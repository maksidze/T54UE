#include "T5Scene.h"
#include "T5Bridge.h"
#include "T5MenuStyle.h"
#include "Dom/JsonObject.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Input/Reply.h"
#include "InputCoreTypes.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"

class ST5Dialogue final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(ST5Dialogue) {}
        SLATE_ARGUMENT(AT5Scene*, Scene)
        SLATE_ARGUMENT(TSharedPtr<FT5UIResources>, Resources)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args)
    {
        Scene = Args._Scene;
        Resources = Args._Resources;
        const auto SpeechPanel = T5MenuArt(Resources, TEXT("DialoguePanel"));
        const auto TradePanel = T5MenuArt(Resources, TEXT("ItemListPanel"));
        const auto ShowSpeaker = [this] { return Trade || SpeakerName.IsEmpty() ? EVisibility::Collapsed : EVisibility::HitTestInvisible; };
        ChildSlot
        [
            SNew(SConstraintCanvas)
            + SConstraintCanvas::Slot().Anchors(FAnchors(.38f, .45f, .565f, .55f)).Offset(FMargin(0))
            [
                SNew(SBox).HAlign(HAlign_Right).VAlign(VAlign_Center).Visibility_Lambda(ShowSpeaker)
                [SNew(STextBlock).Text_Lambda([this] { return SpeakerName; }).Font(T5MenuFont(Resources, 25, TEXT("Dialogue")))
                    .ShadowOffset(FVector2D(1.5, 1.5)).ShadowColorAndOpacity(FLinearColor::Black)]
            ]
            + SConstraintCanvas::Slot().Anchors_Lambda([this]
                {
                    return Trade ? FAnchors(.024f, .10f, .35f, .86f) : FAnchors(.582f, .27f, .92f, .73f);
                }).Offset(FMargin(0))
            [
                SNew(SBorder).Padding(FMargin(15, 15, 23, 15)).VAlign(VAlign_Center)
                .BorderImage_Lambda([this, SpeechPanel, TradePanel] { return Trade ? TradePanel.Get() : SpeechPanel.Get(); })
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight()
                    [
                        SNew(SVerticalBox).Visibility_Lambda([this] { return Trade ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed; })
                        + SVerticalBox::Slot().AutoHeight().Padding(12, 0, 0, 14)
                        [SNew(STextBlock).Text_Lambda([this] { return SpeakerName; }).Font(T5MenuFont(Resources, 29))]
                        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
                        [T5MenuBar(Resources, TEXT("ItemBottom"), SAssignNew(Balance, STextBlock).Font(T5MenuFont(Resources, 17)))]
                        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
                        [
                            SNew(SHorizontalBox)
                            + SHorizontalBox::Slot().FillWidth(1)
                            [
                                SNew(SButton).ButtonStyle(T5MenuButtonStyle()).IsFocusable(false).HAlign(HAlign_Center)
                                .OnClicked_Lambda([this] { return SelectTradeTab(false); })
                                [SNew(STextBlock).Text(NSLOCTEXT("T5Trade", "Buy", "КУПИТЬ")).Font(T5MenuFont(Resources, 22, TEXT("Dialogue")))
                                    .ColorAndOpacity_Lambda([this] { return FSlateColor(TabColor(!bSelling)); })]
                            ]
                            + SHorizontalBox::Slot().FillWidth(1)
                            [
                                SNew(SButton).ButtonStyle(T5MenuButtonStyle()).IsFocusable(false).HAlign(HAlign_Center)
                                .OnClicked_Lambda([this] { return SelectTradeTab(true); })
                                [SNew(STextBlock).Text(NSLOCTEXT("T5Trade", "Sell", "ПРОДАТЬ")).Font(T5MenuFont(Resources, 22, TEXT("Dialogue")))
                                    .ColorAndOpacity_Lambda([this] { return FSlateColor(TabColor(bSelling)); })]
                            ]
                        ]
                    ]
                    + SVerticalBox::Slot().AutoHeight()
                    [SNew(SBox).MaxDesiredHeight(340)[SAssignNew(Choices, SScrollBox)]]
                    + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
                    [SAssignNew(TradeMessage, STextBlock).Font(T5MenuFont(Resources, 18))]
                ]
            ]
            + SConstraintCanvas::Slot().Anchors(FAnchors(.19f, .79f, .81f, .90f)).Offset(FMargin(0))
            [
                SNew(SBox).HAlign(HAlign_Center).VAlign(VAlign_Center)
                [
                    SAssignNew(Line, STextBlock).AutoWrapText(true).Justification(ETextJustify::Center)
                    .Font(T5MenuFont(Resources, 24, TEXT("Dialogue"))).ShadowOffset(FVector2D(1.5, 2)).ShadowColorAndOpacity(FLinearColor::Black)
                ]
            ]
            + SConstraintCanvas::Slot().Anchors(FAnchors(.68f, .91f, .97f, .98f)).Offset(FMargin(0))
            [
                SNew(SBox).HAlign(HAlign_Right).VAlign(VAlign_Center)
                [
                    SNew(SButton).ButtonStyle(T5MenuButtonStyle()).IsFocusable(false).OnClicked(this, &ST5Dialogue::Close)
                    [
                        SNew(STextBlock).Font(T5MenuFont(Resources, 19, TEXT("Dialogue"))).ShadowOffset(FVector2D(1, 1)).ShadowColorAndOpacity(FLinearColor::Black)
                        .Text_Lambda([this] {
                            return FText::FromString(Trade ? TEXT("[Esc]  К разговору") : TEXT("[Esc]  Выйти"));
                        })
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

        SpeakerName = FText::FromString(Data->GetStringField(TEXT("name")));
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
            SAssignNew(Row, SButton).ButtonStyle(T5MenuButtonStyle()).IsFocusable(false).ContentPadding(FMargin(2, 4)).HAlign(HAlign_Fill)
            .ButtonColorAndOpacity_Lambda([this, Index] { return TabColor(Index == Selected); })
            .OnHovered_Lambda([this, Index] { Selected = Index; })
            .OnClicked_Lambda([this, Id] { return Choose(Id); })
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
                [
                    SNew(SBox).WidthOverride(16).HeightOverride(16)
                    [SNew(SImage).Image(T5MenuArt(Resources, TEXT("DialogueNext"), false))
                        .Visibility_Lambda([this, Index] { return Selected == Index ? EVisibility::HitTestInvisible : EVisibility::Hidden; })
                        .RenderTransform(FSlateRenderTransform(FQuat2D(-UE_HALF_PI))).RenderTransformPivot(FVector2D(.5, .5))]
                ]
                + SHorizontalBox::Slot().FillWidth(1)
                [
                    SNew(STextBlock).Text(FText::FromString(Label)).AutoWrapText(true).Font(T5MenuFont(Resources, 22, TEXT("Dialogue")))
                        .ShadowOffset(FVector2D(1, 1.5)).ShadowColorAndOpacity(FLinearColor::Black)
                        .ColorAndOpacity_Lambda([this, Index] {
                            return FSlateColor(Index == Selected ? FLinearColor::White : FLinearColor(.72f, .72f, .72f));
                        })
                ]
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
            [SNew(STextBlock).Text(NSLOCTEXT("T5Trade", "Empty", "Нет подходящих товаров"))
                .Font(T5MenuFont(Resources, 19, TEXT("Dialogue")))];
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
    FText SpeakerName;
    TSharedPtr<FT5UIResources> Resources;
    TSharedPtr<STextBlock> Line, Balance, TradeMessage;
    TSharedPtr<FJsonObject> Trade;
    bool bSelling = false;
    TSharedPtr<SScrollBox> Choices;
    TArray<uint32> ChoiceIds;
    TArray<TSharedPtr<SButton>> ChoiceRows;
    FString LastState;
    uint32 Actor = 0;
    int32 Selected = 0;
};

#if WITH_DEV_AUTOMATION_TESTS
TSharedRef<SWidget> MakeT5DialogueReview(const TSharedPtr<FT5UIResources>& Resources, bool bTrade)
{
    auto Menu = SNew(ST5Dialogue).Scene(nullptr).Resources(Resources).Visibility(EVisibility::HitTestInvisible);
    if (bTrade)
    {
        Menu->Update(TEXT(R"({"actor":1,"name":"Лукан Валерий","trade":{
            "playerGold":125,"vendorGold":750,"message":"","buy":[
            [1,"Зелье лечения",3,34],[2,"Стальная стрела",12,3],[3,"Железный меч",1,76]],
            "sell":[]}})"));
    }
    else
    {
        Menu->Update(TEXT(R"({"actor":1,"name":"Лукан Валерий","line":"Добро пожаловать в «Ривервудского торговца».",
            "choices":[[1,"Что у тебя есть на продажу?"],[2,"Что случилось?"]]})"));
    }
    return Menu;
}
#endif

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

    auto Menu = SNew(ST5Dialogue).Scene(this).Resources(UIResources);
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
