#include "T5Scene.h"
#include "T5Bridge.h"
#include "T5Coordinates.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/PlayerController.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"

class ST5Inventory final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(ST5Inventory) {}
        SLATE_ARGUMENT(AT5Scene*, Scene)
        SLATE_ARGUMENT(uint32, Owner)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args)
    {
        Scene = Args._Scene;
        Owner = Args._Owner;
        ChildSlot.HAlign(HAlign_Center).VAlign(VAlign_Center)
        [
            SNew(SBorder).Padding(20).BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
            .BorderBackgroundColor(FLinearColor(.025f, .035f, .048f, .97f))
            [
                SNew(SBox).WidthOverride(560)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
                    [SAssignNew(Title, STextBlock).Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 20))]
                    + SVerticalBox::Slot().AutoHeight()
                    [SNew(SBox).MaxDesiredHeight(400)[SAssignNew(Rows, SScrollBox)]]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 0)
                    [SNew(SButton).Text(FText::FromString(TEXT("Закрыть (Esc)"))).OnClicked_Lambda([this] { return Close(); })]
                ]
            ]
        ];
    }

    bool Refresh()
    {
        auto* Host = Scene.Get();
        TArray<TSharedPtr<FJsonValue>> Entries;
        if (!Host || !Host->Papyrus || !Host->Papyrus->Inventory(Owner, Entries)) return false;
        const bool bPlayer = Owner == 0x14;
        Title->SetText(FText::FromString(bPlayer ? TEXT("Инвентарь") : Entries[0]->AsString()));
        Rows->ClearChildren();
        for (int32 Index = 1; Index < Entries.Num(); ++Index)
        {
            const auto& Item = Entries[Index]->AsArray();
            if (Item.Num() != 5) continue;
            const uint32 Id = Item[0]->AsNumber();
            const int32 Count = Item[1]->AsNumber();
            const FString Name = Item[2]->AsString();
            const bool bEquipped = Item[4]->AsBool();
            Rows->AddSlot().Padding(0, 1)
            [
                SNew(SButton).Text(FText::FromString(FString::Printf(TEXT("%s  ×%d%s"),
                    *Name, Count, bEquipped ? TEXT("  ·  экипировано") : TEXT(""))))
                .ToolTipText(FText::FromString(bPlayer ? TEXT("Использовать или сменить экипировку") : TEXT("Взять")))
                .OnClicked_Lambda([this, Id, Count] {
                    if (auto* Host = Scene.Get(); Host && Host->Papyrus)
                    {
                        if (!Host->Papyrus->InventoryAction(Owner, Id, Count))
                            Host->SaveMessage(TEXT("Этот предмет пока нельзя использовать"));
                        if (!Refresh()) return Close();
                    }
                    return FReply::Handled();
                })
            ];
        }
        if (Entries.Num() == 1)
            Rows->AddSlot().Padding(4)[SNew(STextBlock).Text(FText::FromString(TEXT("Пусто")))];
        return true;
    }

    bool SupportsKeyboardFocus() const override { return true; }

    FReply OnKeyDown(const FGeometry&, const FKeyEvent& Event) override
    {
        const auto Key = Event.GetKey();
        return Key == EKeys::Escape || Key == EKeys::Tab || Key == EKeys::I ? Close() : FReply::Unhandled();
    }

private:
    FReply Close()
    {
        if (auto* Host = Scene.Get()) Host->CloseInventory();
        return FReply::Handled();
    }

    TWeakObjectPtr<AT5Scene> Scene;
    uint32 Owner = 0;
    TSharedPtr<STextBlock> Title;
    TSharedPtr<SScrollBox> Rows;
};

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
    auto Menu = SNew(ST5Inventory).Scene(this).Owner(Container);
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
