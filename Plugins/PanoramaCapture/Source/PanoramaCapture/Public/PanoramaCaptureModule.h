#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class FPanoramaCaptureModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    /** Returns true when the NVENC runtime is available on the current machine. */
    static bool IsNvencAvailable();

private:
    void RegisterSettings();
    void UnregisterSettings();

    bool bNvencAvailable = false;
};
