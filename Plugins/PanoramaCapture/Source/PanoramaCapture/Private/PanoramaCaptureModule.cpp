#include "PanoramaCaptureModule.h"

#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"
#include "PanoramaCaptureSettings.h"
#include "ShaderCore.h"
#include "Misc/Paths.h"
#if WITH_EDITOR
#include "ISettingsModule.h"
#endif

#define LOCTEXT_NAMESPACE "FPanoramaCaptureModule"

static TWeakPtr<ISettingsSection> GPanoramaCaptureSettingsSection;

void FPanoramaCaptureModule::StartupModule()
{
    const FString ShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("PanoramaCapture"))->GetBaseDir(), TEXT("Shaders"));
    AddShaderSourceDirectoryMapping(TEXT("/PanoramaCapture"), ShaderDir);

#if PLATFORM_WINDOWS
    void* NvEncDllHandle = FPlatformProcess::GetDllHandle(TEXT("nvEncodeAPI64.dll"));
    if (NvEncDllHandle)
    {
        bNvencAvailable = true;
        FPlatformProcess::FreeDllHandle(NvEncDllHandle);
    }
    else
    {
        bNvencAvailable = false;
    }
#else
    bNvencAvailable = false;
#endif

    RegisterSettings();
}

void FPanoramaCaptureModule::ShutdownModule()
{
    UnregisterSettings();

    if (FModuleManager::Get().IsModuleLoaded("ShaderCore"))
    {
        ResetAllShaderSourceDirectoryMappings();
    }
}

bool FPanoramaCaptureModule::IsNvencAvailable()
{
#if PLATFORM_WINDOWS
    static bool bChecked = false;
    static bool bAvailable = false;
    if (!bChecked)
    {
        bChecked = true;
        void* NvEncDllHandle = FPlatformProcess::GetDllHandle(TEXT("nvEncodeAPI64.dll"));
        if (NvEncDllHandle)
        {
            bAvailable = true;
            FPlatformProcess::FreeDllHandle(NvEncDllHandle);
        }
    }
    return bAvailable;
#else
    return false;
#endif
}

void FPanoramaCaptureModule::RegisterSettings()
{
#if WITH_EDITOR
    if (ISettingsModule* SettingsModule = FModuleManager::LoadModulePtr<ISettingsModule>("Settings"))
    {
        GPanoramaCaptureSettingsSection = SettingsModule->RegisterSettings(
            TEXT("Project"),
            TEXT("Plugins"),
            TEXT("PanoramaCapture"),
            LOCTEXT("PanoramaCaptureSettings", "Panorama Capture"),
            LOCTEXT("PanoramaCaptureSettingsDescription", "Configure default options for the panorama capture plugin."),
            GetMutableDefault<UPanoramaCaptureSettings>());
    }
#endif
}

void FPanoramaCaptureModule::UnregisterSettings()
{
#if WITH_EDITOR
    if (GPanoramaCaptureSettingsSection.IsValid())
    {
        if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
        {
            SettingsModule->UnregisterSettings(TEXT("Project"), TEXT("Plugins"), TEXT("PanoramaCapture"));
        }
        GPanoramaCaptureSettingsSection.Reset();
    }
#endif
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FPanoramaCaptureModule, PanoramaCapture)
