#include "ModelContextProtocolModule.h"

#include "UnrealMCPServer.h"

IMPLEMENT_MODULE(FModelContextProtocolModule, ModelContextProtocol)

void FModelContextProtocolModule::StartupModule()
{
    Server = MakeShared<FUnrealMCPServer, ESPMode::ThreadSafe>();
    Server->Start();
}

void FModelContextProtocolModule::ShutdownModule()
{
    if (Server)
    {
        Server->Stop();
        Server.Reset();
    }
}
