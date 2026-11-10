#include "T5Scene.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "T5Audio.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

class FT5AudioSmoke final : public IAutomationLatentCommand
{
public:
    FT5AudioSmoke(FAutomationTestBase& InTest, AT5Scene& InScene) : Test(InTest), Scene(&InScene) {}

    bool Update() override
    {
        const auto* Loader = Scene.Get();
        if (Loader && Loader->RuntimeAudio)
        {
            bCarriage |= Loader->RuntimeAudio->HasMixed(TEXT("/obj_carriage_move_lpm.wav"));
            bDialogue |= Loader->RuntimeAudio->HasMixed(TEXT("/mq101__00101aea_1.fuz"));
            bIntroMusic |= Loader->RuntimeAudio->HasMixed(TEXT("/mus_special_cartintro_01.xwm"));
            bDungeonMusic |= Loader->RuntimeAudio->HasMixed(TEXT("/mus_dungeon_01.xwm"))
                || Loader->RuntimeAudio->HasMixed(TEXT("/mus_dungeon_02.xwm"))
                || Loader->RuntimeAudio->HasMixed(TEXT("/mus_dungeon_03.xwm"));
        }
        if (!bCarriage || !bDialogue || !bIntroMusic || !bDungeonMusic)
            if (Loader && GetCurrentRunTime() < 150)
                return false;
        Test.TestTrue(TEXT("Audio mixer consumed the original MQ101 cart loop"), bCarriage);
        Test.TestTrue(TEXT("Audio mixer consumed the original first MQ101 voiced response"), bDialogue);
        Test.TestTrue(TEXT("Audio mixer consumed the original cart music"), bIntroMusic);
        Test.TestTrue(TEXT("Audio mixer switched to an original dungeon music track at Helgen"), bDungeonMusic);
        return true;
    }

private:
    FAutomationTestBase& Test;
    TWeakObjectPtr<AT5Scene> Scene;
    bool bCarriage = false;
    bool bDialogue = false;
    bool bIntroMusic = false;
    bool bDungeonMusic = false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5MQ101AudioTest, "T5Data.MQ101Audio",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FT5MQ101AudioTest::RunTest(const FString&)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("T5AudioSmoke")))
    {
        AddInfo(TEXT("Pass -T5AudioSmoke with the original MQ101 profile and an enabled audio device."));
        return true;
    }
    for (const auto& Context : GEngine->GetWorldContexts())
        if (UWorld* World = Context.World(); World && World->IsGameWorld())
            for (TActorIterator<AT5Scene> It(World); It; ++It)
            {
                FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FT5AudioSmoke>(*this, **It));
                return true;
            }
    AddError(TEXT("Start a T5Data game world to run the audio test."));
    return false;
}
#endif
