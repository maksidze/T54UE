#include "T5Scene.h"
#include "T5PlayerPawn.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "T5Bridge.h"
#include "T5CharacterCreator.h"
#include "T5Compass.h"
#include "T5UIResources.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Fonts/FontCache.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Rendering/SlateRenderer.h"
#include "UnrealClient.h"

TSharedRef<SWidget> MakeT5ContainerReview(AT5Scene& Scene, const TSharedPtr<FT5UIResources>& Resources);
TSharedRef<SWidget> MakeT5DialogueReview(const TSharedPtr<FT5UIResources>& Resources, bool bTrade);

class FT5UISmoke final : public IAutomationLatentCommand
{
public:
    FT5UISmoke(FAutomationTestBase& InTest, AT5Scene& InScene, FString InOutput)
        : Test(InTest), Scene(&InScene), Output(MoveTemp(InOutput)) {}

    bool Update() override
    {
        auto* S = Scene.Get();
        const double Now = FPlatformTime::Seconds();
        if (!S || S->bLoadingFailed || Now - Started > 180)
        {
            Test.AddError(FString::Printf(TEXT("UI review stopped at step %d"), Step));
            return true;
        }
        if (!Capture.IsEmpty())
        {
            if (Now < NextStep)
                return false;
            if (!bScreenshotRequested)
            {
                if (Step <= 7 && S->Compass)
                    Test.TestEqual(TEXT("Compass visibility at capture"), S->Compass->GetVisibility().IsVisible(), Step <= 1);
                if (Step == 0)
                    Test.TestTrue(TEXT("Fallback screenshot used no imported art"), S->Compass->PaintedArtForReview().IsEmpty());
                if (Step == 1)
                {
                    const auto& Painted = S->Compass->PaintedArtForReview();
                    for (FName Art : {FName(TEXT("CompassFrame")), FName(TEXT("QuestMarker")),
                            FName(TEXT("QuestDoor")), FName(TEXT("Crosshair")), FName(TEXT("ResourceMeter")),
                            FName(TEXT("HealthFill")), FName(TEXT("MagickaFill")), FName(TEXT("StaminaFill"))})
                    {
                        if (Test.TestTrue(Art.ToString() + TEXT(" reached Slate painting"), Painted.Contains(Art)))
                            Test.AddInfo(TEXT("T5_UI_PAINTED ") + Art.ToString());
                    }
                }
                VerifyMenuArt(*S);
                FScreenshotRequest::RequestScreenshot(Capture, true, false);
                bScreenshotRequested = true;
                return false;
            }
            if (FScreenshotRequest::IsScreenshotRequested())
                return false;
            Test.TestTrue(Capture, IFileManager::Get().FileSize(*Capture) > 0);
            Capture.Reset();
            S->UIResources->TrackMenuArtForReview();
        }
        if (!S->Papyrus || S->bSceneLoading || S->bStartupPreparing || S->bRestoringCheckpoint)
            return false;

        switch (Step)
        {
        case -2:
            if (!S->Compass || (S->UIResources && !S->UIResources->IsReady()))
                return false;
            if (!VerifyImport(*S))
                return true;
            if (!Test.TestTrue(TEXT("Placed camera in the Riverwood Trader"), PlaceCamera(*S)))
                return true;
            NextStep = Now + 1.0;
            ++Step;
            return false;
        case -1:
            if (Now < NextStep)
                return false;
            UGameplayStatics::SetGamePaused(S->GetWorld(), true);
            S->Compass->Update(0, {{-28, FVector2D(.43, .39)}, {32, FVector2D(.59, .40), true}}, true);
            S->Compass->SetResourcePools(FVector3f(.88f, .77f, .92f), true);
            S->Compass->UseImportedArtForReview(false);
            return Take(TEXT("00-hud-fallback"));
        case 0:
            S->Compass->UseImportedArtForReview(true);
            return Take(TEXT("01-hud-compass"));
        case 1:
            UGameplayStatics::SetGamePaused(S->GetWorld(), false);
            S->OpenPlayerInventory();
            Test.TestTrue(TEXT("Player inventory opened"), S->InventoryMenu.IsValid());
            return Take(TEXT("02-inventory"));
        case 2:
            S->CloseInventory();
            UGameplayStatics::SetGamePaused(S->GetWorld(), true);
            S->bCompassVisible = false;
            ShowPreview(*S, MakeT5ContainerReview(*S, S->UIResources));
            return Take(TEXT("03-container"));
        case 3:
            RemovePreview(*S);
            UGameplayStatics::SetGamePaused(S->GetWorld(), false);
            S->ToggleMapBrowser();
            Test.TestTrue(TEXT("Location browser opened"), S->MapBrowser.IsValid());
            return Take(TEXT("04-location-browser"));
        case 4:
            S->CloseMapBrowser();
            S->OpenSaveBrowser();
            Test.TestTrue(TEXT("Save browser opened"), S->SaveBrowser.IsValid());
            return Take(TEXT("05-save-browser"));
        case 5:
            S->CloseSaveBrowser();
            S->OpenRaceMenu();
            ++Step;
            return false;
        case 6:
            if (!S->CharacterCreator || !S->CharacterCreator->bReady)
                return false;
            Test.TestTrue(TEXT("Race menu built its character preview"), S->CharacterCreator->Preview.IsValid());
            return Take(TEXT("06-race-menu"));
        case 7:
            S->CharacterCreator->Close();
            S->CharacterCreator.Reset();
            PlaceCamera(*S);
            ShowPreview(*S, MakeT5DialogueReview(S->UIResources, false));
            return Take(TEXT("07-dialogue"));
        case 8:
            ShowPreview(*S, MakeT5DialogueReview(S->UIResources, true));
            return Take(TEXT("08-trade"));
        default:
            RemovePreview(*S);
            return true;
        }
    }

private:
    void VerifyMenuArt(const AT5Scene& S)
    {
        const auto Check = [&](std::initializer_list<const TCHAR*> Symbols)
        {
            for (const TCHAR* Symbol : Symbols)
                if (Test.TestTrue(FPaths::GetBaseFilename(Capture) + TEXT(" uses ") + Symbol,
                        S.UIResources->MenuArtForReview().Contains(Symbol)))
                    Test.AddInfo(TEXT("T5_UI_MENU_ART ") + FPaths::GetBaseFilename(Capture) + TEXT(" ") + Symbol);
        };
        switch (Step)
        {
        case 2:
        case 3:
            Check({TEXT("ItemCategoryPanel"), TEXT("ItemListPanel"), TEXT("ItemCard"), TEXT("ItemBottom")});
            break;
        case 4:
            Check({TEXT("MapTown")});
            break;
        case 5:
            Check({TEXT("JournalPanel"), TEXT("JournalDivider"), TEXT("JournalBottom")});
            break;
        case 7:
            Check({TEXT("RaceHeader"), TEXT("RacePanel"), TEXT("RaceBottom"),
                TEXT("RaceSliderTrack"), TEXT("RaceSliderThumb")});
            break;
        case 8:
            Check({TEXT("DialoguePanel"), TEXT("DialogueNext")});
            break;
        case 9:
            Check({TEXT("ItemListPanel"), TEXT("ItemBottom")});
            break;
        }
    }

    bool VerifyImport(AT5Scene& S)
    {
        if (!Test.TestTrue(TEXT("Interface catalog contains archive entries"),
                S.UIResources && S.UIResources->GetEntryCount() > 0))
            return false;
        auto& FontCache = *FSlateApplication::Get().GetRenderer()->GetFontCache();
        const FString& Config = S.UIResources->FontConfigForReview();
        FString Sample = TEXT("W");
        if (Config.EndsWith(TEXT("_ru.txt"))) Sample += TEXT("Жф");
        if (Config.EndsWith(TEXT("_pl.txt"))) Sample += TEXT("Łą");
        if (Config.EndsWith(TEXT("_ja.txt"))) Sample += TEXT("日あ");
        if (Config.EndsWith(TEXT("_cn.txt"))) Sample += TEXT("中龍");
        for (const TCHAR* Role : {TEXT("Regular"), TEXT("Medium"), TEXT("Bold"), TEXT("Dialogue")})
        {
            const auto Font = S.UIResources->GetFont(Role, 24);
            const FCompositeFont* Composite = Font.GetCompositeFont();
            const FTypefaceEntry* Source = Composite ? Composite->DefaultTypeface.Fonts.FindByPredicate(
                [Role](const FTypefaceEntry& Entry) { return Entry.Name == Role; }) : nullptr;
            auto& Characters = FontCache.GetCharacterList(Font, 1.f);
            for (const TCHAR Code : Sample)
            {
                const auto& Glyph = Characters.GetCharacter(Code, EFontFallback::FF_NoFallback);
                if (!Test.TestTrue(FString::Printf(TEXT("Imported %s font rendered %c"), Role, Code),
                        Glyph.Valid && Glyph.GlyphIndex != 0 && Glyph.USize > 0 && Glyph.VSize > 0
                        && Source && Glyph.FontData && *Glyph.FontData == Source->Font && Source->Font.GetFontFaceAsset()))
                    return false;
            }
        }
        for (FName Name : {FName(TEXT("CompassFrame")), FName(TEXT("QuestMarker")), FName(TEXT("QuestDoor")),
                FName(TEXT("PlayerMarker")), FName(TEXT("EnemyMarker")), FName(TEXT("DoorMarker")), FName(TEXT("Crosshair")),
                FName(TEXT("MapDoor")), FName(TEXT("MapSelection")), FName(TEXT("ItemEquipped"))})
        {
            const FSlateBrush* Brush = S.UIResources->GetBrush(Name);
            if (!Test.TestTrue(Name.ToString(), Brush && Brush->GetResourceObject() && Brush->ImageSize.X > 0 && Brush->ImageSize.Y > 0))
                return false;
        }
        return true;
    }

    bool Take(const TCHAR* Name)
    {
        Capture = Output / (FString(Name) + TEXT(".png"));
        bScreenshotRequested = false;
        NextStep = FPlatformTime::Seconds() + 1.0;
        ++Step;
        return false;
    }

    bool PlaceCamera(AT5Scene& S)
    {
        FTransform Pose;
        uint32 Parent = 0;
        auto* Controller = S.GetWorld()->GetFirstPlayerController();
        auto* Pawn = Controller ? Controller->GetPawnOrSpectator() : nullptr;
        if (!Pawn || !S.Papyrus->ReferenceTransform(Merchant, Pose, Parent))
            return false;
        const FVector Target = S.GetActorTransform().TransformPosition(Pose.GetLocation());
        const FVector Front = S.GetActorTransform().TransformVectorNoScale(Pose.GetRotation().GetForwardVector());
        const FVector Eye = Target + Front * 190 + FVector(0, 0, S.Papyrus->PlayerEyeHeight());
        const FRotator View = (Target + FVector(0, 0, 140) - Eye).Rotation();
        if (auto* Player = Cast<AT5PlayerPawn>(Pawn))
            Player->PlaceFeet(Player->Feet() + Eye - Player->GetPawnViewLocation());
        else
            Pawn->SetActorLocationAndRotation(Eye, View, false, nullptr, ETeleportType::TeleportPhysics);
        Controller->SetControlRotation(View);
        Controller->SetViewTarget(Pawn);
        if (Controller->PlayerCameraManager)
        {
            Controller->PlayerCameraManager->UpdateCamera(0);
            Controller->PlayerCameraManager->SetGameCameraCutThisFrame();
        }
        return true;
    }

    void ShowPreview(AT5Scene& S, TSharedRef<SWidget> Widget)
    {
        RemovePreview(S);
        S.GetWorld()->GetGameViewport()->AddViewportWidgetContent(Widget, 220);
        Preview = Widget;
    }

    void RemovePreview(AT5Scene& S)
    {
        if (Preview)
        {
            S.GetWorld()->GetGameViewport()->RemoveViewportWidgetContent(Preview.ToSharedRef());
            Preview.Reset();
        }
    }

    static constexpr uint32 Merchant = 0x00013487;
    FAutomationTestBase& Test;
    TWeakObjectPtr<AT5Scene> Scene;
    TSharedPtr<SWidget> Preview;
    FString Output, Capture;
    double Started = FPlatformTime::Seconds(), NextStep = 0;
    int32 Step = -2;
    bool bScreenshotRequested = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5UIReview, "T5Data.UI.Review",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FT5UIReview::RunTest(const FString&)
{
    FString Output;
    if (!FParse::Value(FCommandLine::Get(), TEXT("T5UIReview="), Output))
    {
        AddError(TEXT("Run Tools/TestInterface.ps1 to select a screenshot directory and scene"));
        return false;
    }
    IFileManager::Get().MakeDirectory(*Output, true);
    for (const auto& Context : GEngine->GetWorldContexts())
        if (auto* World = Context.World(); World && World->IsGameWorld())
            for (TActorIterator<AT5Scene> It(World); It; ++It)
            {
                FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FT5UISmoke>(*this, **It, Output));
                return true;
            }
    AddError(TEXT("No runtime T5 scene for UI review"));
    return false;
}
#endif
