#include "T5MapBrowser.h"

#include "T5Scene.h"
#include "T5MenuStyle.h"
#include "InputCoreTypes.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/Text/STextBlock.h"

class ST5CellGrid : public SLeafWidget
{
public:
    DECLARE_DELEGATE_OneParam(FOnIntPointChanged, FIntPoint);
    SLATE_BEGIN_ARGS(ST5CellGrid) {}
        SLATE_ATTRIBUTE(FIntPoint, Cell)
        SLATE_EVENT(FOnIntPointChanged, OnCellPicked)
        SLATE_ARGUMENT(TSharedPtr<FT5UIResources>, Resources)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args)
    {
        Cell = Args._Cell;
        OnCellPicked = Args._OnCellPicked;
        Resources = Args._Resources;
    }

    void SetWorld(const FT5Location* Map, double Scale, FVector2D Offset)
    {
        Runs.Reset();
        Names.Reset();
        Bounds = FIntRect(0, 0, 1, 1);
        MapScale = Scale;
        MapOffset = Offset;
        if (!Map || Map->Cells.IsEmpty())
            return;
        Bounds = Map->MapBounds.Get(FIntRect(Map->MinCell, Map->MaxCell + FIntPoint(1, 1)));
        TArray<FIntPoint> Points;
        for (FIntPoint Point : Map->Cells)
            if (Bounds.Contains(Point))
                Points.Add(Point);
        Points.Sort([](FIntPoint A, FIntPoint B) { return A.Y == B.Y ? A.X < B.X : A.Y < B.Y; });
        for (FIntPoint Point : Points)
        {
            if (!Runs.IsEmpty() && Runs.Last().Min.Y == Point.Y && Runs.Last().Max.X == Point.X)
                ++Runs.Last().Max.X;
            else
                Runs.Emplace(Point, Point + FIntPoint(1, 1));
        }
        for (const auto& Pair : Map->NamedCells)
            if (Bounds.Contains(Pair.Key))
                Names.Add(Pair.Key);
        Invalidate(EInvalidateWidgetReason::Paint);
    }

private:
    FOnIntPointChanged OnCellPicked;
    TAttribute<FIntPoint> Cell;
    TArray<FIntRect> Runs;
    TArray<FIntPoint> Names;
    FIntRect Bounds;
    TSharedPtr<FT5UIResources> Resources;
    double MapScale = 1;
    FVector2D MapOffset = FVector2D::ZeroVector;

    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(265, 300); }

    FVector2D TopLeft(FVector2D Size, double& Step) const
    {
        Step = FMath::Min(Size.X / Bounds.Width(), Size.Y / Bounds.Height());
        return (Size - FVector2D(Bounds.Size()) * Step) / 2;
    }

    virtual int32 OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&,
        FSlateWindowElementList& Out, int32 Layer, const FWidgetStyle&, bool) const override
    {
        const auto* Brush = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
        double Step;
        const FVector2D Corner = TopLeft(Geometry.GetLocalSize(), Step);
        const auto Draw = [&](int32 DrawLayer, FVector2D Position, FVector2D Size, FLinearColor Color, const FSlateBrush* Image = nullptr)
        {
            FSlateDrawElement::MakeBox(Out, DrawLayer, Geometry.ToPaintGeometry(FVector2f(Size),
                FSlateLayoutTransform(FVector2f(Position))), Image ? Image : Brush, ESlateDrawEffect::None, Color);
        };
        Draw(Layer, Corner, FVector2D(Bounds.Size()) * Step, FLinearColor(.02f, .02f, .02f));
        for (const auto& Run : Runs)
            Draw(Layer + 1, Corner + FVector2D(Run.Min.X - Bounds.Min.X, Bounds.Max.Y - Run.Max.Y) * Step,
                FVector2D(Run.Size()) * Step, FLinearColor(.11f, .12f, .11f));
        for (FIntPoint Point : Names)
            Draw(Layer + 2, Corner + FVector2D(Point.X - Bounds.Min.X, Bounds.Max.Y - Point.Y - 1) * Step,
                FVector2D(Step, Step), FLinearColor(.32f, .33f, .29f));
        if (MapScale >= 0 && !Runs.IsEmpty())
        {
            const FVector2D Point = (FVector2D(Cell.Get()) + FVector2D(.5)) * MapScale + MapOffset;
            if (Point.X >= Bounds.Min.X && Point.X < Bounds.Max.X && Point.Y >= Bounds.Min.Y && Point.Y < Bounds.Max.Y)
            {
                const auto* Marker = Resources ? Resources->GetBrush(TEXT("MapSelection")) : nullptr;
                const FVector2D IconSize = Marker ? FVector2D(12, 27) : FVector2D(FMath::Max(5.0, Step));
                Draw(Layer + 3, Corner + FVector2D(Point.X - Bounds.Min.X, Bounds.Max.Y - Point.Y) * Step - IconSize * .5,
                    IconSize, FLinearColor::White, Marker);
            }
        }
        return Layer + 3;
    }

    virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override
    {
        if (Event.GetEffectingButton() != EKeys::LeftMouseButton || MapScale <= 0 || Runs.IsEmpty())
            return FReply::Unhandled();
        double Step;
        const FVector2D Corner = TopLeft(Geometry.GetLocalSize(), Step);
        const FVector2D Grid = (Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition()) - Corner) / Step;
        const FVector2D MapCell(Bounds.Min.X + Grid.X, Bounds.Max.Y - Grid.Y);
        if (Grid.X >= 0 && Grid.Y >= 0 && Grid.X < Bounds.Width() && Grid.Y < Bounds.Height())
        {
            const FVector2D Local = (MapCell - MapOffset) / MapScale;
            if (FMath::Abs(Local.X) < 32767 && FMath::Abs(Local.Y) < 32767)
                OnCellPicked.ExecuteIfBound(FIntPoint(FMath::FloorToInt(Local.X), FMath::FloorToInt(Local.Y)));
            return FReply::Handled();
        }
        return FReply::Unhandled();
    }
};

FString FT5Location::Key() const
{
    return FString::Printf(TEXT("0x%08X"), Id);
}

FString FT5Location::Label() const
{
    return FString::Printf(TEXT("%s   %s   [%s]"), EditorId.IsEmpty() ? *Key() : *EditorId,
        bInterior ? TEXT("Interior") : TEXT("World"), *Plugin);
}

void ST5MapBrowser::Construct(const FArguments&, AT5Scene* InOwner)
{
    Owner = InOwner;
    const auto Resources = InOwner->UIResources;
    const auto Visibility = [this] { return Selected && !Selected->bInterior ? EVisibility::Visible : EVisibility::Collapsed; };
    ChildSlot
    .HAlign(HAlign_Center).VAlign(VAlign_Center)
    [
        SNew(SBorder).Padding(18)
        .BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
        .BorderBackgroundColor(FLinearColor(.014f, .014f, .014f, .91f))
        [
            SNew(SBox).WidthOverride(910).HeightOverride(600)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
                    [SNew(SBox).WidthOverride(22).HeightOverride(27)
                        [SNew(SImage).Image(T5MenuArt(Resources, TEXT("MapTown"), false))]]
                    + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                    [SNew(STextBlock).Text(FText::FromString(TEXT("КАРТА  /  ЛОКАЦИИ")))
                        .Font(T5MenuFont(Resources, 22, TEXT("Regular")))]
                ]
                + SVerticalBox::Slot().AutoHeight()
                [SNew(SSearchBox).HintText(FText::FromString(TEXT("Search world, interior, plugin or FormID"))).OnTextChanged(this, &ST5MapBrowser::Filter)]
                + SVerticalBox::Slot().FillHeight(1).Padding(0, 8)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().FillWidth(1)
                    [
                        SAssignNew(List, SListView<TSharedPtr<FT5Location>>)
                    .ListItemsSource(&Items)
                        .OnSelectionChanged(this, &ST5MapBrowser::Select)
                        .OnGenerateRow_Lambda([Resources](TSharedPtr<FT5Location> Item, const TSharedRef<STableViewBase>& Table)
                        {
                            return SNew(STableRow<TSharedPtr<FT5Location>>, Table).Padding(FMargin(6, 3))
                            [
                                SNew(SHorizontalBox)
                                + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                                [SNew(SBox).WidthOverride(14).HeightOverride(17)
                                    [SNew(SImage).Image(T5MenuArt(Resources, Item->bInterior ? TEXT("MapDoor") : TEXT("MapTown"), false))]]
                                + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                                [SNew(STextBlock).Text(FText::FromString(Item->Label()))
                                    .Font(T5MenuFont(Resources, 13, TEXT("Regular")))]
                            ];
                        })
                    ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(14, 0, 0, 0)
                    [
                        SNew(SBox).Visibility_Lambda(Visibility).WidthOverride(280)
                        [
                            SNew(SVerticalBox)
                            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                            [
                                SNew(STextBlock).Text(FText::FromString(TEXT("World map grid")))
                                .Font(T5MenuFont(Resources, 14, TEXT("Bold")))
                            ]
                            + SVerticalBox::Slot().FillHeight(1)
                            [
                                SAssignNew(Overview, ST5CellGrid)
                                .Resources(Resources)
                                .Cell_Lambda([this] { return FIntPoint(CellX, CellY); })
                                .OnCellPicked_Lambda([this](FIntPoint Cell)
                                {
                                    CellX = Cell.X;
                                    CellY = Cell.Y;
                                })
                            ]
                            + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
                            [
                                SNew(STextBlock).Text_Lambda([this]
                                {
                                    const auto* Name = Selected ? Selected->NamedCells.Find(FIntPoint(CellX, CellY)) : nullptr;
                                    return FText::FromString(Name ? *Name : FString::Printf(TEXT("Cell %d, %d"), CellX, CellY));
                                })
                            ]
                        ]
                    ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [
                    SNew(STextBlock).Text(this, &ST5MapBrowser::Details)
                    .Font(T5MenuFont(Resources, 13, TEXT("Regular")))
                ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock).AutoWrapText(true).Text_Lambda([this]
                    {
                        const auto* Scene = Owner.Get();
                        return Scene ? FText::FromString(Scene->LoadStatus) : FText::GetEmpty();
                    }).Visibility_Lambda([this]
                    {
                        const auto* Scene = Owner.Get();
                        return Scene && !Scene->LoadStatus.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
                    })
                ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
                    [SNew(STextBlock).Visibility_Lambda(Visibility).Text(FText::FromString(TEXT("Cell X")))]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 14, 0)
                    [
                        SNew(SSpinBox<int32>).MinDesiredWidth(65).Visibility_Lambda(Visibility)
                        .MinValue(-32768).MaxValue(32767).Value_Lambda([this] { return CellX; })
                        .OnValueChanged_Lambda([this](int32 Value) { CellX = Value; })
                    ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
                    [SNew(STextBlock).Visibility_Lambda(Visibility).Text(FText::FromString(TEXT("Cell Y")))]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        SNew(SSpinBox<int32>).MinDesiredWidth(65).Visibility_Lambda(Visibility)
                        .MinValue(-32768).MaxValue(32767).Value_Lambda([this] { return CellY; })
                        .OnValueChanged_Lambda([this](int32 Value) { CellY = Value; })
                    ]
                    + SHorizontalBox::Slot().FillWidth(1) [SNew(SSpacer)]
                    + SHorizontalBox::Slot().AutoWidth().Padding(8, 0)
                    [
                        SNew(SButton).ButtonStyle(T5MenuButtonStyle()).Text(FText::FromString(TEXT("Загрузить"))).OnClicked(this, &ST5MapBrowser::Load)
                        .IsEnabled_Lambda([this] { return Selected.IsValid(); })
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        SNew(SButton).ButtonStyle(T5MenuButtonStyle()).Text(FText::FromString(TEXT("[Esc]  Назад"))).OnClicked_Lambda([this]
                        {
                            if (auto* Scene = Owner.Get())
                                Scene->CloseMapBrowser();
                            return FReply::Handled();
                        })
                    ]
                ]
            ]
        ]
    ];
    Refresh();
}

void ST5MapBrowser::Refresh()
{
    Filter(FText::FromString(Query));
}

void ST5MapBrowser::Filter(const FText& Text)
{
    Query = Text.ToString();
    Items.Reset();
    const auto* Scene = Owner.Get();
    if (Scene)
        for (const auto& Pair : Scene->Locations)
            if (Query.IsEmpty() || Pair.Value->Label().Contains(Query) || Pair.Value->Key().Contains(Query))
                Items.Add(Pair.Value);
    Items.Sort([](const TSharedPtr<FT5Location>& A, const TSharedPtr<FT5Location>& B)
    {
        if (A->bInterior != B->bInterior)
            return !A->bInterior;
        const int32 Order = A->EditorId.Compare(B->EditorId, ESearchCase::IgnoreCase);
        return Order == 0 ? A->Id < B->Id : Order < 0;
    });
    if (Selected && !Items.Contains(Selected))
        List->ClearSelection();
    List->RequestListRefresh();
    if (!Selected && !Items.IsEmpty())
    {
        const auto* Current = Scene->Locations.Find(Scene->ActiveWorld);
        const auto Item = Query.IsEmpty() && Current ? *Current : Items[0];
        List->SetSelection(Item);
        List->RequestScrollIntoView(Item);
    }
}

void ST5MapBrowser::Select(TSharedPtr<FT5Location> Location, ESelectInfo::Type)
{
    Selected = MoveTemp(Location);
    RefreshOverview();
    if (!Selected)
        return;
    CellX = Selected->SuggestedCell.X;
    CellY = Selected->SuggestedCell.Y;
    if (const auto* Scene = Owner.Get(); Scene && Scene->ActiveWorld == Selected->Id)
    {
        CellX = Scene->RequestedCell.X;
        CellY = Scene->RequestedCell.Y;
    }
}

void ST5MapBrowser::RefreshOverview()
{
    if (!Selected || Selected->bInterior || !Overview)
        return;
    const auto* Scene = Owner.Get();
    const FT5Location* Current = Selected.Get();
    double Scale = 1;
    FVector2D Offset = FVector2D::ZeroVector;
    TSet<uint32> Seen;
    while (Scene && Current->Id != Selected->Map && !Seen.Contains(Current->Id))
    {
        Seen.Add(Current->Id);
        const auto* Parent = Scene->Locations.Find(Current->Parent);
        if (!Parent)
            break;
        if (Current->MapScale < 0 || Scale < 0)
            Scale = -1;
        else
        {
            Scale *= Current->MapScale;
            Offset = Offset * Current->MapScale + Current->MapOffset;
        }
        Current = Parent->Get();
    }
    Overview->SetWorld(Current, Scale, Offset);
}

FText ST5MapBrowser::Details() const
{
    const auto* Scene = Owner.Get();
    if (!Scene)
        return FText::GetEmpty();
    if (!Selected)
        return FText::FromString(Scene->CatalogStatus);
    if (Selected->bInterior)
        return FText::FromString(FString::Printf(TEXT("%s   Interior cell"), *Selected->Key()));
    const auto Name = [Scene](uint32 Id)
    {
        const auto* Location = Scene->Locations.Find(Id);
        return Location ? ((*Location)->EditorId.IsEmpty() ? (*Location)->Key() : (*Location)->EditorId) : FString(TEXT("None"));
    };
    return FText::FromString(FString::Printf(TEXT("%s   %d cells   Parent %s   Map %s   LOD %s   Land %s"),
        *Selected->Key(), Selected->Cells.Num(), *Name(Selected->Parent), *Name(Selected->Map), *Name(Selected->Lod), *Name(Selected->Land)));
}

FReply ST5MapBrowser::Load()
{
    if (auto* Scene = Owner.Get(); Scene && Selected)
    {
        const uint32 Id = Selected->Id;
        Scene->LoadLocation(Id, FIntPoint(CellX, CellY), true);
        Scene->CloseMapBrowser();
    }
    return FReply::Handled();
}

FReply ST5MapBrowser::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event)
{
    if (Event.GetKey() == EKeys::Escape)
    {
        if (auto* Scene = Owner.Get())
            Scene->CloseMapBrowser();
        return FReply::Handled();
    }
    return SCompoundWidget::OnKeyDown(Geometry, Event);
}
