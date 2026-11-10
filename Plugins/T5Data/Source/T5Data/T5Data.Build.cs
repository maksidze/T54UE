using System.IO;
using UnrealBuildTool;

public class T5Data : ModuleRules
{
    public T5Data(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
        PrivateDependencyModuleNames.AddRange(new[] {
            "Chaos", "ChaosCore", "ImageCore", "ImageWrapper", "Json", "MeshDescription", "PhysicsCore", "Projects",
            "InputCore", "RenderCore", "RHI", "Slate", "SlateCore", "StaticMeshDescription"
        });
        AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib");
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            string Root = Path.Combine(PluginDirectory, "Binaries", "ThirdParty", "Win64");
            RuntimeDependencies.Add(Path.Combine(Root, "..."), StagedFileType.NonUFS);
            string RaceMenu = Path.Combine(PluginDirectory, "Resources", "RaceMenu");
            if (Directory.Exists(RaceMenu))
                RuntimeDependencies.Add(Path.Combine(RaceMenu, "..."), StagedFileType.NonUFS);
            RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Resources", "IntroEnvironment.json"), StagedFileType.NonUFS);
            RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Resources", "SkyrimPlugins.txt"), StagedFileType.NonUFS);
        }
    }
}

