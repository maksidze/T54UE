#include "T5Scene.h"
#include "T5Bridge.h"
#include "Dom/JsonObject.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Input/Reply.h"
#include "InputCoreTypes.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"

class ST5Dialogue final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(ST5Dialogue) {}
        SLATE_ARGUMENT(AT5Scene*, Scene)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args)
    {
        Scene = Args._Scene;
        ChildSlot.HAlign(HAlign_Right).VAlign(VAlign_Bottom).Padding(24, 24, 48, 56)
        [
            SNew(SBorder).Padding(22).BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
            .BorderBackgroundColor(FLinearColor(.025f, .032f, .043f, .96f))
            [
                SNew(SBox).WidthOverride(550)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
                    [
                        SAssignNew(Name, STextBlock)
                        .Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 21))
                        .ColorAndOpacity(FLinearColor(.95f, .82f, .60f))
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
                    [
                        SAssignNew(Line, STextBlock).WrapTextAt(550)
                        .Font(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 15))
                    ]
                    + SVerticalBox::Slot().AutoHeight()
                    [
                        SNew(SVerticalBox).Visibility_Lambda([this] {
                            return Trade ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed;
                        })
                        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 9)
                        [SAssignNew(Balance, STextBlock).ColorAndOpacity(FLinearColor(.95f, .82f, .60f))]
                        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                        [
                            SNew(SHorizontalBox)
                            + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 4, 0)
                            [
                                SNew(SButton).IsFocusable(false).HAlign(HAlign_Center)
                                .Text(NSLOCTEXT("T5Trade", "Buy", "Купить"))
                                .ButtonColorAndOpacity_Lambda([this] { return TabColor(!bSelling); })
                                .OnClicked_Lambda([this] { return SelectTradeTab(false); })
                            ]
                            + SHorizontalBox::Slot().FillWidth(1).Padding(4, 0, 0, 0)
                            [
                                SNew(SButton).IsFocusable(false).HAlign(HAlign_Center)
                                .Text(NSLOCTEXT("T5Trade", "Sell", "Продать"))
                                .ButtonColorAndOpacity_Lambda([this] { return TabColor(bSelling); })
                                .OnClicked_Lambda([this] { return SelectTradeTab(true); })
                            ]
                        ]
                    ]
                    + SVerticalBox::Slot().AutoHeight()
                    [SNew(SBox).MaxDesiredHeight(290)[SAssignNew(Choices, SScrollBox)]]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
                    [SAssignNew(TradeMessage, STextBlock).ColorAndOpacity(FLinearColor(.95f, .82f, .60f))]
                    + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0, 12, 0, 0)
                    [
                        SNew(SButton).IsFocusable(false).Text_Lambda([this] {
                            return Trade ? NSLOCTEXT("T5Trade", "Back", "Назад к разговору (Esc)")
                                : NSLOCTEXT("T5Dialogue", "Close", "Закончить разговор (Esc)");
                        })
                        .OnClicked(this, &ST5Dialogue::Close)
                    ]
                ]
            ]
        ];
    }

    bool Update(FString State, uint32 ExpectedActor = 0)
    {
        if (State.IsEmpty()) return false;
        if (State == LastState) return true;

        TSharedPtr<FJsonObject> Data;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(State), Data) || !Data) return false;
        const uint32 Speaker = uint32(Data->GetNumberField(TEXT("actor")));
        if (!Speaker || (ExpectedActor && Speaker != ExpectedActor) || (Actor && Speaker != Actor)) return false;
        Actor = Speaker;
        LastState = MoveTemp(State);

        Name->SetText(FText::FromString(Data->GetStringField(TEXT("name"))));
        const bool bWasTrading = Trade.IsValid();
        Trade = Data->HasTypedField<EJson::Object>(TEXT("trade")) ? Data->GetObjectField(TEXT("trade")) : nullptr;
        if (Trade)
        {
            Line->SetVisibility(EVisibility::Collapsed);
            if (!bWasTrading) bSelling = false;
            RefreshTrade(bWasTrading);
        }
        else
        {
            TradeMessage->SetVisibility(EVisibility::Collapsed);
            const FString Response = Data->GetStringField(TEXT("line"));
            Line->SetText(FText::FromString(Response));
            Line->SetVisibility(Response.IsEmpty() ? EVisibility::Collapsed : EVisibility::SelfHitTestInvisible);
            ResetRows();
            for (const auto& Value : Data->GetArrayField(TEXT("choices")))
            {
                const auto& Entry = Value->AsArray();
                if (Entry.Num() == 2) AddRow(uint32(Entry[0]->AsNumber()), Entry[1]->AsString());
            }
            Choices->ScrollToStart();
        }
        return true;
    }

    bool SupportsKeyboardFocus() const override { return true; }

    FReply OnKeyDown(const FGeometry&, const FKeyEvent& Event) override
    {
        const auto Key = Event.GetKey();
        if (Key == EKeys::Escape || Key == EKeys::Gamepad_FaceButton_Right) return Close();
        if (Trade)
        {
            if (Key == EKeys::Tab) return SelectTradeTab(!bSelling);
            if (Key == EKeys::Left || Key == EKeys::Gamepad_DPad_Left) return SelectTradeTab(false);
            if (Key == EKeys::Right || Key == EKeys::Gamepad_DPad_Right) return SelectTradeTab(true);
        }
        if (ChoiceIds.IsEmpty()) return FReply::Handled();
        if (Key == EKeys::Enter || Key == EKeys::Gamepad_FaceButton_Bottom)
            return Event.IsRepeat() ? FReply::Handled() : Choose(ChoiceIds[Selected]);
        const bool bUp = Key == EKeys::Up || Key == EKeys::Gamepad_DPad_Up;
        if (bUp || Key == EKeys::Down || Key == EKeys::Gamepad_DPad_Down)
        {
            Selected = FMath::Clamp(Selected + (bUp ? -1 : 1), 0, ChoiceIds.Num() - 1);
            Choices->ScrollDescendantIntoView(ChoiceRows[Selected]);
        }
        return FReply::Handled();
    }

private:
    void ResetRows()
    {
        Choices->ClearChildren();
        ChoiceIds.Reset();
        ChoiceRows.Reset();
        Selected = 0;
    }

    static FLinearColor TabColor(bool bSelected)
    {
        return bSelected ? FLinearColor(.82f, .66f, .42f) : FLinearColor(.49f, .54f, .62f);
    }

    void AddRow(uint32 Id, const FString& Label)
    {
        const int32 Index = ChoiceIds.Add(Id);
        TSharedPtr<SButton> Row;
        Choices->AddSlot().Padding(0, 2)
        [
            SAssignNew(Row, SButton).IsFocusable(false).ContentPadding(FMargin(12, 7))
            .ButtonColorAndOpacity_Lambda([this, Index] { return TabColor(Index == Selected); })
            .OnClicked_Lambda([this, Id] { return Choose(Id); })
            [
                SNew(STextBlock).Text(FText::FromString(Label)).WrapTextAt(500)
                .Font(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 13))
            ]
        ];
        ChoiceRows.Add(Row);
    }

    void RefreshTrade(bool bKeepPosition)
    {
        const int32 PreviousSelection = Selected;
        const float PreviousScroll = Choices->GetScrollOffset();
        ResetRows();
        Balance->SetText(FText::Format(NSLOCTEXT("T5Trade", "Gold", "Ваше золото: {0}     Золото торговца: {1}"),
            int32(Trade->GetNumberField(TEXT("playerGold"))), int32(Trade->GetNumberField(TEXT("vendorGold")))));
        const FString Message = Trade->GetStringField(TEXT("message"));
        TradeMessage->SetText(FText::FromString(Message));
        TradeMessage->SetVisibility(Message.IsEmpty() ? EVisibility::Collapsed : EVisibility::SelfHitTestInvisible);
        for (const auto& Value : Trade->GetArrayField(bSelling ? TEXT("sell") : TEXT("buy")))
        {
            const auto& Entry = Value->AsArray();
            if (Entry.Num() != 4) continue;
            AddRow(uint32(Entry[0]->AsNumber()), FString::Printf(TEXT("%s  ×%d   ·   %d зол."),
                *Entry[1]->AsString(), int32(Entry[2]->AsNumber()), int32(Entry[3]->AsNumber())));
        }
        if (ChoiceIds.IsEmpty())
        {
            Choices->AddSlot().Padding(8)
            [SNew(STextBlock).Text(NSLOCTEXT("T5Trade", "Empty", "Нет подходящих товаров"))];
            Choices->ScrollToStart();
        }
        else if (bKeepPosition)
        {
            Selected = FMath::Clamp(PreviousSelection, 0, ChoiceIds.Num() - 1);
            Choices->SetScrollOffset(PreviousScroll);
        }
        else Choices->ScrollToStart();
    }

    FReply SelectTradeTab(bool bSell)
    {
        if (bSelling != bSell)
        {
            bSelling = bSell;
            RefreshTrade(false);
        }
        return FReply::Handled();
    }

    FReply Send(ET5DialogueAction Action, uint32 Id = 0)
    {
        if (auto* Host = Scene.Get(); Host && Host->Papyrus)
        {
            Host->Papyrus->DialogueAction(Action, Id);
            Host->TickPresentation();
            Host->TickDialogue();
        }
        return FReply::Handled();
    }

    FReply Choose(uint32 Id)
    {
        return Send(Trade ? (bSelling ? ET5DialogueAction::Sell : ET5DialogueAction::Buy)
            : ET5DialogueAction::Choose, Id);
    }

    FReply Close()
    {
        if (Trade) return Send(ET5DialogueAction::LeaveTrade);
        if (auto* Host = Scene.Get()) Host->CloseDialogue();
        return FReply::Handled();
    }

    TWeakObjectPtr<AT5Scene> Scene;
    TSharedPtr<STextBlock> Name, Line, Balance, TradeMessage;
    TSharedPtr<FJsonObject> Trade;
    bool bSelling = false;
    TSharedPtr<SScrollBox> Choices;
    TArray<uint32> ChoiceIds;
    TArray<TSharedPtr<SButton>> ChoiceRows;
    FString LastState;
    uint32 Actor = 0;
    int32 Selected = 0;
};

void AT5Scene::OpenDialogue(uint32 Actor)
{
    if (!Papyrus || DialogueMenu) return;
    auto* World = GetWorld();
    auto* Controller = World ? World->GetFirstPlayerController() : nullptr;
    auto* Viewport = World ? World->GetGameViewport() : nullptr;
    if (!Controller || !Viewport || InventoryMenu || MapBrowser || SaveBrowser || CharacterCreator
        || bRestoringCheckpoint || (bSceneLoading && bFullLoadingScreen))
    {
        Papyrus->DialogueAction(ET5DialogueAction::Close);
        return;
    }

    auto Menu = SNew(ST5Dialogue).Scene(this);
    if (!Menu->Update(Papyrus->DialogueState(), Actor))
    {
        Papyrus->DialogueAction(ET5DialogueAction::Close);
        return;
    }
    DialogueMenu = Menu;
    DialogueController = Controller;
    Viewport->AddViewportWidgetContent(Menu, 220);
    FInputModeUIOnly Mode;
    Mode.SetWidgetToFocus(Menu);
    Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
    Controller->SetInputMode(Mode);
    Controller->FlushPressedKeys();
    Controller->SetIgnoreMoveInput(true);
    Controller->SetIgnoreLookInput(true);
    Controller->bShowMouseCursor = true;
    UE_LOG(LogTemp, Display, TEXT("T5_DIALOGUE_OPEN actor=%08X"), Actor);
}

void AT5Scene::CloseDialogue()
{
    if (!DialogueMenu) return;
    auto Menu = MoveTemp(DialogueMenu);
    if (Papyrus) Papyrus->DialogueAction(ET5DialogueAction::Close);
    if (auto* World = GetWorld())
        if (auto* Viewport = World->GetGameViewport())
            Viewport->RemoveViewportWidgetContent(Menu.ToSharedRef());
    if (auto* Controller = DialogueController.Get())
    {
        Controller->SetInputMode(FInputModeGameOnly());
        Controller->FlushPressedKeys();
        Controller->SetIgnoreMoveInput(false);
        Controller->SetIgnoreLookInput(false);
        Controller->bShowMouseCursor = false;
    }
    DialogueController.Reset();
    UE_LOG(LogTemp, Display, TEXT("T5_DIALOGUE_CLOSE"));
}

void AT5Scene::TickDialogue()
{
    if (DialogueMenu && (!DialogueController.IsValid() || !Papyrus || !DialogueMenu->Update(Papyrus->DialogueState())))
        CloseDialogue();
}
