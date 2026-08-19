#include "UnrealMCPModule.h"

#include "UnrealMCPServer.h"

IMPLEMENT_MODULE(FUnrealMCPModule, UnrealMCP)

void FUnrealMCPModule::StartupModule()
{
    Server = MakeUnique<FUnrealMCPServer>();
    Server->Start();
}

void FUnrealMCPModule::ShutdownModule()
{
    if (Server)
    {
        Server->Stop();
        Server.Reset();
    }
}
