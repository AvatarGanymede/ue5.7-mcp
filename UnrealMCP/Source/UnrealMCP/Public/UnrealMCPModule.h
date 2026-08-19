#pragma once

#include "Modules/ModuleManager.h"

class FUnrealMCPServer;

class FUnrealMCPModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    TUniquePtr<FUnrealMCPServer> Server;
};
