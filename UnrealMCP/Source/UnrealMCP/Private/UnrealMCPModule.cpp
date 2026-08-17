#include "UnrealMCPModule.h"

#include "UnrealMCPWorkerServer.h"

IMPLEMENT_MODULE(FUnrealMCPModule, UnrealMCP)

void FUnrealMCPModule::StartupModule()
{
    WorkerServer = MakeUnique<FUnrealMCPWorkerServer>();
    WorkerServer->Start();
}

void FUnrealMCPModule::ShutdownModule()
{
    if (WorkerServer)
    {
        WorkerServer->Stop();
        WorkerServer.Reset();
    }
}
