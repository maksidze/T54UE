#include "T5Scene.h"
#include "T5Bridge.h"
#include "T5Coordinates.h"
#include "T5MenuStyle.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/PlayerController.h"
#include "PreviewScene.h"
#include "UnrealClient.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBackgroundBlur.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
    enum class EItemCategory : uint8 { All, Weapons, Apparel, Potions, Scrolls, Food, Ingredients, Books, Keys, Misc, Count };
    constexpr const TCHAR* CategoryNames[] = {TEXT("ВСЁ"), TEXT("ОРУЖИЕ"), TEXT("ОДЕЖДА"), TEXT("ЗЕЛЬЯ"),
        TEXT("СВИТКИ"), TEXT("ЕДА"), TEXT("ИНГРЕДИЕНТЫ"), TEXT("КНИГИ"), TEXT("КЛЮЧИ"), TEXT("РАЗНОЕ")};

    constexpr uint32 ItemType(const ANSICHAR (&Name)[5])
    {
        return uint32(Name[0]) | (uint32(Name[1]) << 8) | (uint32(Name[2]) << 16) | (uint32(Name[3]) << 24);
    }

    struct FInventoryRow
    {
        uint32 Id = 0;
        int32 Count = 0;
        FString Name;
        EItemCategory Category = EItemCategory::Misc;
        TOptional<double> Weight, Value, Damage;
        bool bEquipped = false;

        static FInventoryRow Read(const TArray<TSharedPtr<FJsonValue>>& Entry)
        {
            FInventoryRow Item;
            Item.Id = uint32(Entry[0]->AsNumber());
            Item.Count = int32(Entry[1]->AsNumber());
            Item.Name = Entry[2]->AsString();
            Item.bEquipped = Entry[4]->AsBool();
            bool bFood = false;
            if (Entry.Num() > 5)
            {
                const TSharedPtr<FJsonObject>* Data = nullptr;
                if (Entry[5]->TryGetObject(Data))
                {
                    (*Data)->TryGetBoolField(TEXT("food"), bFood);
                    double Number = 0;
                    if ((*Data)->TryGetNumberField(TEXT("weight"), Number)) Item.Weight = Number;
                    if ((*Data)->TryGetNumberField(TEXT("value"), Number)) Item.Value = Number;
                    if ((*Data)->TryGetNumberField(TEXT("damage"), Number)) Item.Damage = Number;
                }
            }
            switch (uint32(Entry[3]->AsNumber()))
            {
            case ItemType("WEAP"): case ItemType("AMMO"): Item.Category = EItemCategory::Weapons; break;
            case ItemType("ARMO"): Item.Category = EItemCategory::Apparel; break;
            case ItemType("ALCH"): Item.Category = bFood ? EItemCategory::Food : EItemCategory::Potions; break;
            case ItemType("SCRL"): Item.Category = EItemCategory::Scrolls; break;
            case ItemType("INGR"): Item.Category = EItemCategory::Ingredients; break;
            case ItemType("BOOK"): Item.Category = EItemCategory::Books; break;
            case ItemType("KEYM"): Item.Category = EItemCategory::Keys; break;
            }
            return Item;
        }
    };

    UTexture2D* RenderInventoryModel(UStaticMesh& Mesh)
    {
        const auto Bounds = Mesh.GetBounds();
        if (!FMath::IsFinite(Bounds.SphereRadius) || Bounds.SphereRadius < .01)
            return nullptr;

        FPreviewScene Preview(FPreviewScene::ConstructionValues().SetEditor(false).SetCreatePhysicsScene(false)
            .SetTransactional(false).SetLightRotation(FRotator(-24, -150, 0)).SetLightBrightness(4));
        auto* Component = NewObject<UStaticMeshComponent>();
        Component->SetStaticMesh(&Mesh);
        Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        const FQuat Rotation = FRotator(0, 35, -14).Quaternion();
        Preview.AddComponent(Component, FTransform(Rotation, -Rotation.RotateVector(Bounds.Origin)));

        constexpr int32 Resolution = 384;
        auto* Target = NewObject<UTextureRenderTarget2D>();
        Target->RenderTargetFormat = RTF_RGBA16f;
        Target->ClearColor = FLinearColor(0, 0, 0, 1);
        Target->InitAutoFormat(Resolution, Resolution);
        Target->UpdateResourceImmediate(true);

        auto* Capture = NewObject<USceneCaptureComponent2D>();
        Capture->bCaptureEveryFrame = false;
        Capture->bCaptureOnMovement = false;
        Capture->CaptureSource = SCS_SceneColorHDR;
        Capture->TextureTarget = Target;
        Capture->FOVAngle = 30;
        Capture->ShowFlags.SetFog(false);
        Capture->ShowFlags.SetAtmosphere(false);
        Capture->ShowFlags.SetBloom(false);
        Capture->ShowFlags.SetEyeAdaptation(false);
        Capture->ShowFlags.SetMotionBlur(false);
        const FVector Eye = FVector(-1, -.32, .22).GetSafeNormal() * Bounds.SphereRadius * 4.5;
        Preview.AddComponent(Capture, FTransform((-Eye).Rotation(), Eye));
        Preview.GetWorld()->SendAllEndOfFrameUpdates();
        Capture->CaptureScene();

        TArray<FLinearColor> Colors;
        if (!Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Colors) ||
            Colors.Num() != Resolution * Resolution)
            return nullptr;
        TArray<FColor> Pixels;
        Pixels.Reserve(Colors.Num());
        bool bHasModel = false;
        for (FLinearColor Pixel : Colors)
        {
            Pixel.A = 1 - FMath::Clamp(Pixel.A, 0.f, 1.f);
            bHasModel |= Pixel.A > .01f;
            Pixels.Add(Pixel.ToFColorSRGB());
        }
        if (!bHasModel)
            return nullptr;
        auto* Texture = UTexture2D::CreateTransient(Resolution, Resolution, PF_B8G8R8A8, NAME_None,
            MakeArrayView(reinterpret_cast<const uint8*>(Pixels.GetData()), Pixels.Num() * sizeof(FColor)));
        if (Texture)
        {
            Texture->SRGB = true;
            Texture->NeverStream = true;
            Texture->UpdateResource();
        }
        return Texture;
    }
}

class ST5Inventory final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(ST5Inventory) {}
        SLATE_ARGUMENT(AT5Scene*, Scene)
        SLATE_ARGUMENT(uint32, Owner)
        SLATE_ARGUMENT(TSharedPtr<FT5UIResources>, Resources)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args)
    {
        Scene = Args._Scene;
        Owner = Args._Owner;
        Resources = Args._Resources;
        PreviewBrush.DrawAs = ESlateBrushDrawType::Image;
        PreviewBrush.ImageSize = FVector2D(384, 384);
        ChildSlot
        [
            SNew(SConstraintCanvas)
            + SConstraintCanvas::Slot().Anchors(FAnchors(0, 0, 1, 1)).Offset(FMargin(0))
            [T5MenuBackdrop()]
            + SConstraintCanvas::Slot().Anchors(FAnchors(.023f, 0, .175f, .90f)).Offset(FMargin(0))
            [
                SNew(SBorder).BorderImage(T5MenuArt(Resources, TEXT("ItemCategoryPanel")))
                    .Padding(FMargin(16, 18, 28, 18))
                [SAssignNew(CategoryRows, SScrollBox).BackPadScrolling(true).FrontPadScrolling(true)
                    .ScrollBarVisibility(EVisibility::Collapsed)]
            ]
            + SConstraintCanvas::Slot().Anchors(FAnchors(.034f, .11f, .15f, .19f)).Offset(FMargin(0))
            [
                SAssignNew(Title, STextBlock).Font(T5MenuFont(Resources, 25)).AutoWrapText(true)
                    .Visibility(EVisibility::HitTestInvisible)
            ]
            + SConstraintCanvas::Slot().Anchors(FAnchors(.187f, 0, .385f, .90f)).Offset(FMargin(0))
            [
                SNew(SBorder).BorderImage(T5MenuArt(Resources, TEXT("ItemListPanel")))
                    .Padding(FMargin(16, 14, 30, 14))
                [SAssignNew(Rows, SScrollBox).BackPadScrolling(true).FrontPadScrolling(true)
                    .ScrollBarVisibility(EVisibility::Collapsed)]
            ]
            + SConstraintCanvas::Slot().Anchors(FAnchors(.52f, .11f, .84f, .58f)).Offset(FMargin(0))
            [
                SNew(SScaleBox).Stretch(EStretch::ScaleToFit)
                .Visibility_Lambda([this] { return PreviewBrush.GetResourceObject() ? EVisibility::HitTestInvisible : EVisibility::Hidden; })
                [SNew(SImage).Image(&PreviewBrush)]
            ]
            + SConstraintCanvas::Slot().Anchors(FAnchors(.51f, .59f, .86f, .83f)).Offset(FMargin(0))
            [
                SNew(SBorder).BorderImage(T5MenuArt(Resources, TEXT("ItemCard"))).BorderBackgroundColor(FLinearColor(1, 1, 1, .78f)).Padding(FMargin(28, 12, 28, 27))
                    .VAlign(VAlign_Center).Clipping(EWidgetClipping::ClipToBounds)
                    .Visibility_Lambda([this] { return ItemName.IsEmpty() ? EVisibility::Hidden : EVisibility::HitTestInvisible; })
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0, 0, 0, 12)
                    [SNew(STextBlock).Text_Lambda([this] { return ItemName; }).AutoWrapText(true)
                        .Justification(ETextJustify::Center).Font(T5MenuFont(Resources, 29))]
                    + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
                    [SNew(STextBlock).Text_Lambda([this] { return ItemDetails; }).AutoWrapText(true)
                        .Justification(ETextJustify::Center).Font(T5MenuFont(Resources, 19, TEXT("Regular")))]
                ]
            ]
            + SConstraintCanvas::Slot().Anchors(FAnchors(0, .90f, 1, 1)).Offset(FMargin(0))
            [
                T5MenuBar(Resources, TEXT("ItemBottom"),
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [SNew(STextBlock).Text(FText::FromString(Owner == 0x14 ? TEXT("[Enter]  Использовать") : TEXT("[Enter]  Взять")))
                        .Font(T5MenuFont(Resources, 19, TEXT("Regular")))]
                    + SHorizontalBox::Slot().AutoWidth().Padding(16, 0)
                    [SNew(SButton).ButtonStyle(T5MenuButtonStyle()).OnClicked_Lambda([this] { return Close(); })
                        [SNew(STextBlock).Text(FText::FromString(TEXT("[Esc]  Назад"))).Font(T5MenuFont(Resources, 19, TEXT("Regular")))]]
                    + SHorizontalBox::Slot().FillWidth(1).HAlign(HAlign_Right).VAlign(VAlign_Center)
                    [SNew(STextBlock).Text_Lambda([this] { return PlayerTotals; }).Font(T5MenuFont(Resources, 19, TEXT("Regular")))])
            ]
        ];
        RegisterActiveTimer(.05f, FWidgetActiveTimerDelegate::CreateSP(this, &ST5Inventory::CenterInitialRows));
    }

    bool Refresh()
    {
        auto* Host = Scene.Get();
        TArray<TSharedPtr<FJsonValue>> Entries;
        if (!Host || !Host->Papyrus || !Host->Papyrus->Inventory(Owner, Entries))
            return false;
        if (Owner == 0x14)
            UpdateTotals(Entries);
        else
        {
            TArray<TSharedPtr<FJsonValue>> Player;
            if (Host->Papyrus->Inventory(0x14, Player))
                UpdateTotals(Player);
        }
        Populate(Entries);
        return true;
    }

    void Populate(const TArray<TSharedPtr<FJsonValue>>& Entries)
    {
        Title->SetText(FText::FromString(Owner == 0x14 ? TEXT("ИНВЕНТАРЬ") : Entries[0]->AsString().ToUpper()));
        Items.Reset();
        for (int32 Index = 1; Index < Entries.Num(); ++Index)
            if (const auto& Entry = Entries[Index]->AsArray(); Entry.Num() >= 5)
                Items.Add(FInventoryRow::Read(Entry));
        Items.Sort([](const FInventoryRow& A, const FInventoryRow& B)
        {
            const int32 Order = A.Name.Compare(B.Name, ESearchCase::IgnoreCase);
            return Order ? Order < 0 : A.Id < B.Id;
        });

        Categories.Reset();
        Categories.Add(EItemCategory::All);
        for (uint8 Index = 1; Index < uint8(EItemCategory::Count); ++Index)
            if (Items.ContainsByPredicate([Index](const FInventoryRow& Row) { return uint8(Row.Category) == Index; }))
                Categories.Add(EItemCategory(Index));
        if (!Categories.Contains(Category))
            Category = EItemCategory::All;
        CategoryRows->ClearChildren();
        CategoryWidgets.Reset();
        for (EItemCategory Entry : Categories)
        {
            TSharedPtr<SButton> Button;
            CategoryRows->AddSlot()
            [
                SAssignNew(Button, SButton).IsFocusable(false).ButtonStyle(T5MenuButtonStyle()).HAlign(HAlign_Fill).ContentPadding(FMargin(5, 7))
                    .OnClicked_Lambda([this, Entry] { bBrowsingCategories = false; SetCategory(Entry); return FReply::Handled(); })
                [
                    SNew(STextBlock).Text(FText::FromString(CategoryNames[uint8(Entry)])).Font(T5MenuFont(Resources, 21))
                    .ColorAndOpacity_Lambda([this, Entry] { return FSlateColor(Entry == Category ? FLinearColor::White : FLinearColor(.68f, .68f, .68f)); })
                ]
            ];
            CategoryWidgets.Add(Button);
        }
        if (bListsLaidOut)
            CenterCategory();
        RebuildRows();
    }

    bool SupportsKeyboardFocus() const override { return true; }
    FReply OnKeyDown(const FGeometry&, const FKeyEvent& Event) override
    {
        const FKey Key = Event.GetKey();
        if (Key == EKeys::Escape || Key == EKeys::Tab || Key == EKeys::I || Key == EKeys::Gamepad_FaceButton_Right)
            return Close();
        if (Key == EKeys::Left || Key == EKeys::Gamepad_DPad_Left)
            bBrowsingCategories = true;
        else if (Key == EKeys::Right || Key == EKeys::Gamepad_DPad_Right)
            bBrowsingCategories = false;
        else if (Key == EKeys::Up || Key == EKeys::Down || Key == EKeys::Gamepad_DPad_Up || Key == EKeys::Gamepad_DPad_Down)
        {
            const int32 Direction = Key == EKeys::Up || Key == EKeys::Gamepad_DPad_Up ? -1 : 1;
            if (bBrowsingCategories)
                SetCategory(Categories[FMath::Clamp(Categories.IndexOfByKey(Category) + Direction, 0, Categories.Num() - 1)]);
            else if (!VisibleItems.IsEmpty())
            {
                const int32 Row = FMath::Clamp(VisibleItems.IndexOfByKey(Selected) + Direction, 0, VisibleItems.Num() - 1);
                Select(VisibleItems[Row]);
                Rows->ScrollDescendantIntoView(RowWidgets[Row], false, EDescendantScrollDestination::Center);
            }
        }
        else if (Key == EKeys::Enter || Key == EKeys::Gamepad_FaceButton_Bottom)
        {
            if (Event.IsRepeat()) return FReply::Handled();
            if (!bBrowsingCategories && Items.IsValidIndex(Selected)) return Activate(Selected);
            bBrowsingCategories = false;
        }
        else
            return FReply::Unhandled();
        return FReply::Handled();
    }

private:
    void SetCategory(EItemCategory NewCategory)
    {
        if (Category == NewCategory)
            return;
        Category = NewCategory;
        Selected = INDEX_NONE;
        CenterCategory();
        RebuildRows();
    }

    void CenterCategory()
    {
        const int32 Row = Categories.IndexOfByKey(Category);
        if (CategoryWidgets.IsValidIndex(Row))
            CategoryRows->ScrollDescendantIntoView(CategoryWidgets[Row], false, EDescendantScrollDestination::Center);
    }

    void CenterItem()
    {
        const int32 Row = VisibleItems.IndexOfByKey(Selected);
        if (RowWidgets.IsValidIndex(Row))
            Rows->ScrollDescendantIntoView(RowWidgets[Row], false, EDescendantScrollDestination::Center);
    }

    EActiveTimerReturnType CenterInitialRows(double, float)
    {
        // ScrollBox computes its leading padding from the previous layout geometry.
        if (Rows->GetTickSpaceGeometry().GetLocalSize().Y <= 0
            || CategoryRows->GetTickSpaceGeometry().GetLocalSize().Y <= 0)
            return EActiveTimerReturnType::Continue;
        bListsLaidOut = true;
        CenterCategory();
        CenterItem();
        return EActiveTimerReturnType::Stop;
    }

    void RebuildRows()
    {
        const uint32 Previous = PreviewItem;
        VisibleItems.Reset();
        RowWidgets.Reset();
        Rows->ClearChildren();
        int32 Preferred = INDEX_NONE;
        for (int32 Index = 0; Index < Items.Num(); ++Index)
        {
            const auto& Item = Items[Index];
            if (Category != EItemCategory::All && Category != Item.Category)
                continue;
            VisibleItems.Add(Index);
            if (Item.Id == Previous) Preferred = Index;
            const FString Label = Item.Count > 1 ? FString::Printf(TEXT("%s (%d)"), *Item.Name, Item.Count) : Item.Name;
            TSharedPtr<SButton> Button;
            Rows->AddSlot()
            [
                SAssignNew(Button, SButton).IsFocusable(false).ButtonStyle(T5MenuButtonStyle()).ContentPadding(FMargin(5, 5)).HAlign(HAlign_Fill)
                .OnHovered_Lambda([this, Index] { bBrowsingCategories = false; Select(Index); })
                .ToolTipText(FText::FromString(Owner == 0x14 ? TEXT("Использовать или сменить экипировку") : TEXT("Взять")))
                .OnClicked_Lambda([this, Index] { return Activate(Index); })
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
                    [SNew(SBox).WidthOverride(15).HeightOverride(15)
                        [SNew(SImage).Visibility(Item.bEquipped ? EVisibility::HitTestInvisible : EVisibility::Hidden)
                            .Image(T5MenuArt(Resources, TEXT("ItemEquipped"), false))]]
                    + SHorizontalBox::Slot().FillWidth(1)
                    [SNew(STextBlock).Text(FText::FromString(Label)).Font(T5MenuFont(Resources, 19, TEXT("Regular")))
                        .OverflowPolicy(ETextOverflowPolicy::Ellipsis)
                        .ShadowOffset(FVector2D(1, 1)).ShadowColorAndOpacity(FLinearColor::Black)
                        .ColorAndOpacity_Lambda([this, Index] { return FSlateColor(Selected == Index ? FLinearColor::White : FLinearColor(.68f, .68f, .68f)); })]
                ]
            ];
            RowWidgets.Add(Button);
        }
        if (VisibleItems.IsEmpty())
            Rows->AddSlot().Padding(8)[SNew(STextBlock).Text(FText::FromString(TEXT("Пусто"))).Font(T5MenuFont(Resources, 19, TEXT("Regular")))];
        Select(Preferred != INDEX_NONE ? Preferred : VisibleItems.IsEmpty() ? INDEX_NONE : VisibleItems[0]);
        if (bListsLaidOut)
            CenterItem();
    }

    void Select(int32 Index)
    {
        Selected = Index;
        ItemName = ItemDetails = FText::GetEmpty();
        const FInventoryRow* Item = Items.IsValidIndex(Index) ? &Items[Index] : nullptr;
        if (Item)
        {
            ItemName = FText::FromString(Item->Name.ToUpper());
            TArray<FString> Stats;
            const auto Add = [&](const TCHAR* Label, const TOptional<double>& Value)
            {
                if (Value)
                    Stats.Add(FString(Label) + TEXT("  ") + FText::AsNumber(*Value).ToString());
            };
            Add(TEXT("УРОН"), Item->Damage);
            Add(TEXT("ВЕС"), Item->Weight);
            Add(TEXT("ЦЕНА"), Item->Value);
            if (Item->bEquipped) Stats.Add(TEXT("ЭКИПИРОВАНО"));
            ItemDetails = FText::FromString(FString::Join(Stats, TEXT("     ")));
        }

        const uint32 Id = Item ? Item->Id : 0;
        if (PreviewItem == Id)
            return;
        PreviewItem = Id;
        PreviewBrush.SetResourceObject(nullptr);
        if (!Id)
            return;
        if (const auto* Cached = ModelImages.Find(Id))
            PreviewBrush.SetResourceObject(Cached->Get());
        else
        {
            PreviewDue = FPlatformTime::Seconds() + .14;
            if (!PreviewTimer.IsValid())
                PreviewTimer = RegisterActiveTimer(.05f, FWidgetActiveTimerDelegate::CreateSP(this, &ST5Inventory::UpdatePreview));
        }
        Invalidate(EInvalidateWidgetReason::Paint | EInvalidateWidgetReason::Visibility);
    }

    EActiveTimerReturnType UpdatePreview(double, float)
    {
        if (FPlatformTime::Seconds() < PreviewDue)
            return EActiveTimerReturnType::Continue;
        PreviewTimer.Reset();
        if (const auto* Existing = ModelImages.Find(PreviewItem))
        {
            PreviewBrush.SetResourceObject(Existing->Get());
            return EActiveTimerReturnType::Stop;
        }
        UTexture2D* Texture = nullptr;
        if (auto* Host = Scene.Get(); PreviewItem && Host && Host->Papyrus && FApp::CanEverRender())
            if (const auto Data = Host->Papyrus->InventoryModel(PreviewItem); Data && Data->Error.IsEmpty())
            {
                auto Prop = Host->BuildAnimationProp(*Data);
                if (auto* Part = Prop.Component.Get())
                {
                    if (UStaticMesh* Mesh = Part->GetStaticMesh())
                        Texture = RenderInventoryModel(*Mesh);
                    Part->DestroyComponent();
                }
            }
        if (ModelImages.Num() >= 16) ModelImages.Reset();
        if (PreviewItem) ModelImages.Add(PreviewItem, TStrongObjectPtr<UTexture2D>(Texture));
        PreviewBrush.SetResourceObject(Texture);
        Invalidate(EInvalidateWidgetReason::Paint | EInvalidateWidgetReason::Visibility);
        if (Texture)
            UE_LOG(LogTemp, Display, TEXT("T5_ITEM_PREVIEW item=%08X"), PreviewItem);
        return EActiveTimerReturnType::Stop;
    }

    void UpdateTotals(const TArray<TSharedPtr<FJsonValue>>& Entries)
    {
        int32 Gold = 0;
        double Weight = 0;
        bool bHasWeights = Entries.Num() > 1;
        for (int32 Index = 1; Index < Entries.Num(); ++Index)
        {
            const auto& Entry = Entries[Index]->AsArray();
            if (Entry.Num() < 5) continue;
            const auto Item = FInventoryRow::Read(Entry);
            if (Item.Id == 0xf) Gold = Item.Count;
            bHasWeights &= Item.Weight.IsSet();
            Weight += Item.Weight.Get(0) * Item.Count;
        }
        FString Text = FString::Printf(TEXT("ЗОЛОТО  %d"), Gold);
        if (bHasWeights) Text = TEXT("ВЕС  ") + FText::AsNumber(Weight).ToString() + TEXT("      ") + Text;
        PlayerTotals = FText::FromString(Text);
    }

    FReply Activate(int32 Index)
    {
        Select(Index);
        if (auto* Host = Scene.Get(); Host && Host->Papyrus)
        {
            const auto& Item = Items[Index];
            if (!Host->Papyrus->InventoryAction(Owner, Item.Id, Item.Count))
                Host->SaveMessage(TEXT("Этот предмет пока нельзя использовать"));
            if (!Refresh()) return Close();
        }
        return FReply::Handled();
    }

    FReply Close()
    {
        if (auto* Host = Scene.Get()) Host->CloseInventory();
        return FReply::Handled();
    }

    TWeakObjectPtr<AT5Scene> Scene;
    uint32 Owner = 0, PreviewItem = 0;
    int32 Selected = INDEX_NONE;
    EItemCategory Category = EItemCategory::All;
    bool bBrowsingCategories = true;
    bool bListsLaidOut = false;
    FText ItemName, ItemDetails, PlayerTotals;
    TArray<FInventoryRow> Items;
    TArray<EItemCategory> Categories;
    TArray<int32> VisibleItems;
    TArray<TSharedPtr<SButton>> RowWidgets, CategoryWidgets;
    TMap<uint32, TStrongObjectPtr<UTexture2D>> ModelImages;
    FSlateBrush PreviewBrush;
    double PreviewDue = 0;
    TWeakPtr<FActiveTimerHandle> PreviewTimer;
    TSharedPtr<FT5UIResources> Resources;
    TSharedPtr<STextBlock> Title;
    TSharedPtr<SScrollBox> Rows, CategoryRows;
};

#if WITH_DEV_AUTOMATION_TESTS
TSharedRef<SWidget> MakeT5ContainerReview(AT5Scene& Scene, const TSharedPtr<FT5UIResources>& Resources)
{
    auto Menu = SNew(ST5Inventory).Scene(&Scene).Owner(1).Resources(Resources).Visibility(EVisibility::HitTestInvisible);
    const auto Item = [](uint32 Id, const ANSICHAR (&Type)[5], const TCHAR* Name, int32 Count, double Weight, double Value)
    {
        auto Details = MakeShared<FJsonObject>();
        Details->SetNumberField(TEXT("weight"), Weight);
        Details->SetNumberField(TEXT("value"), Value);
        return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
            MakeShared<FJsonValueNumber>(Id), MakeShared<FJsonValueNumber>(Count),
            MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueNumber>(ItemType(Type)),
            MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueObject>(Details)});
    };
    Menu->Populate({MakeShared<FJsonValueString>(TEXT("Сундук")),
        Item(0x12eb7, "WEAP", TEXT("Железный меч"), 1, 9, 25),
        Item(0x3eadd, "ALCH", TEXT("Зелье лечения"), 2, .5, 17),
        Item(0x0f, "MISC", TEXT("Золото"), 27, 0, 1)});
    return Menu;
}
#endif

void AT5Scene::OpenPlayerInventory()
{
    OpenInventory(0x14);
}

void AT5Scene::OpenInventory(uint32 Container)
{
    if (!Papyrus || InventoryMenu || DialogueMenu || MapBrowser || SaveBrowser || CharacterCreator || bRestoringCheckpoint
        || (bSceneLoading && bFullLoadingScreen)) return;
    auto* Controller = GetWorld()->GetFirstPlayerController();
    auto* Viewport = GetWorld()->GetGameViewport();
    if (!Controller || !Viewport) return;
    auto Menu = SNew(ST5Inventory).Scene(this).Owner(Container).Resources(UIResources);
    if (!Menu->Refresh()) return;
    InventoryMenu = Menu;
    OpenInventoryOwner = Container;
    UE_LOG(LogTemp, Display, TEXT("T5_INVENTORY_OPEN owner=%08X"), Container);
    Viewport->AddViewportWidgetContent(Menu, 220);
    bInventoryWasPaused = Controller->IsPaused();
    if (!bInventoryWasPaused) Controller->SetPause(true);
    FInputModeGameAndUI Mode;
    Mode.SetWidgetToFocus(Menu);
    Mode.SetHideCursorDuringCapture(false);
    Controller->SetInputMode(Mode);
    Controller->bShowMouseCursor = true;
}

void AT5Scene::CloseInventory()
{
    if (!InventoryMenu) return;
    auto Menu = MoveTemp(InventoryMenu);
    OpenInventoryOwner = 0;
    if (auto* World = GetWorld())
    {
        if (auto* Viewport = World->GetGameViewport()) Viewport->RemoveViewportWidgetContent(Menu.ToSharedRef());
        if (auto* Controller = World->GetFirstPlayerController())
        {
            Controller->SetInputMode(FInputModeGameOnly());
            Controller->bShowMouseCursor = false;
            if (!bInventoryWasPaused) Controller->SetPause(false);
        }
    }
}

uint32 AT5Scene::FindInteractionReference(const FVector& Start, const FVector& End, bool bIncludeLoot) const
{
    float Closest = 1;
    uint32 Result = 0;
    for (const auto& [Id, Parts] : ReferenceVisuals)
    {
        if (Id == 0x14) continue;
        for (const auto& Part : Parts)
        {
            const auto* Mesh = Cast<UPrimitiveComponent>(Part.Component.Get());
            if (!Mesh || !Mesh->IsVisible() || Part.bColliderOnly
                || (!Cast<USkeletalMeshComponent>(Mesh) && !(bIncludeLoot && LootableReferences.Contains(Id))))
                continue;
            const auto Box = Mesh->Bounds.GetBox();
            FVector Location, Normal;
            float Distance;
            if (FMath::LineExtentBoxIntersection(Box, Start, End, FVector::ZeroVector, Location, Normal, Distance)
                && Distance < Closest)
            {
                Result = Id;
                Closest = Distance;
            }
        }
    }
    return Result;
}

void AT5Scene::ReadyPlayerWeapon()
{
    if (!Papyrus || InventoryMenu || DialogueMenu || MapBrowser || SaveBrowser || CharacterCreator || (bSceneLoading && bFullLoadingScreen)) return;
    const auto Mode = Papyrus->PlayerWeapon(1);
    if (Mode) SaveMessage(Mode == 1 ? TEXT("Оружие наготове") : TEXT("Оружие убрано"));
}

void AT5Scene::AttackPlayerWeapon()
{
    if (!Papyrus || InventoryMenu || DialogueMenu || MapBrowser || SaveBrowser || CharacterCreator || (bSceneLoading && bFullLoadingScreen)) return;
    const auto* Controller = GetWorld()->GetFirstPlayerController();
    float Reach = 0;
    if (!Controller || !Papyrus->PlayerWeapon(0, 0, &Reach)) return;
    FVector Eye; FRotator View; Controller->GetPlayerViewPoint(Eye, View);
    if (Reach < 0)
    {
        const auto Root = GetActorTransform();
        if (Papyrus->FirePlayerBow(UnrealToT5Position(Root.InverseTransformPosition(Eye)),
            T5ToUnrealDirection(Root.InverseTransformVectorNoScale(View.Vector()))))
            PlayViewAttack();
        TickPresentation();
        return;
    }
    const auto End = Eye + View.Vector() * Reach * T5CentimetersPerUnit;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(T5PlayerStrike), true, Controller->GetPawnOrSpectator());
    FHitResult Hit;
    GetWorld()->LineTraceSingleByChannel(Hit, Eye, End, ECC_Visibility, Query);
    const auto Contact = InteractionComponents.FindRef(Hit.GetComponent());
    const auto Target = Contact ? Contact : FindInteractionReference(Eye, Hit.bBlockingHit ? Hit.ImpactPoint : End);
    if (Papyrus->PlayerWeapon(2, Target)) PlayViewAttack();
}
