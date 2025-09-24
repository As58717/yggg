using UnrealBuildTool;

public class PanoramaCapture : ModuleRules
{
    public PanoramaCapture(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "RenderCore",
                "RHI",
                "Projects"
            });

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "Slate",
                "SlateCore",
                "UMG",
                "InputCore",
                "ImageWrapper",
                "ImageWriteQueue",
                "MovieScene",
                "MovieSceneCapture",
                "AudioMixer",
                "MediaUtils",
                "AVEncoder",
                "D3D11RHI",
                "D3D12RHI",
                "Json",
                "JsonUtilities"
            });

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicDefinitions.Add("PANORAMA_CAPTURE_WITH_NVENC=1");
            bEnableExceptions = true;
        }
        else
        {
            PublicDefinitions.Add("PANORAMA_CAPTURE_WITH_NVENC=0");
        }

        // Allow access to shader directory within the plugin.
        AdditionalPropertiesForReceipt.Add("AdditionalShaderDirectory", "$(PluginDir)/Shaders");
    }
}
