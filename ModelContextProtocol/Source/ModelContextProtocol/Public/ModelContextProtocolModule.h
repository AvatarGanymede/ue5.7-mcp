#pragma once

#include "Modules/ModuleManager.h"

class FUnrealMCPServer;

class FModelContextProtocolModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    TSharedPtr<FUnrealMCPServer, ESPMode::ThreadSafe> Server;
};
