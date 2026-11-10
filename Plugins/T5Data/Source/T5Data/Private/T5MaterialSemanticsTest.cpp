#include "T5MaterialSemantics.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5MaterialOpacityTest, "T5Data.Material.VertexOpacity",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FT5MaterialOpacityTest::RunTest(const FString&)
{
    // Pine section from the recorded Helgen approach: shader 0, vertex colors,
    // TREE_ANIM in flags2. Its A=0 vertices must retain texture cutout coverage.
    TestFalse(TEXT("Pine wind weights do not remove needles"), T5UsesVertexOpacity(0x8040030au, 0x20000000u, 0));
    TestFalse(TEXT("Tree shader uses alpha for wind"), T5UsesVertexOpacity(8, 0, 12));
    TestFalse(TEXT("Object LOD alpha is not surface opacity"), T5UsesVertexOpacity(8, 0, 13));
    TestFalse(TEXT("HD object LOD alpha is not surface opacity"), T5UsesVertexOpacity(8, 0, 15));
    TestTrue(TEXT("Ordinary vertex opacity remains enabled"), T5UsesVertexOpacity(8, 0, 0));
    TestFalse(TEXT("Missing vertex colors do not enable opacity"), T5UsesVertexOpacity(0, 0, 0));
    return true;
}
#endif
