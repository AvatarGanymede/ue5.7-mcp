#pragma once

#include "CoreMinimal.h"
#include "HttpRouteHandle.h"

class IHttpRouter;
class FJsonObject;
class FJsonValue;
struct FHttpServerRequest;
struct FHttpServerResponse;

DECLARE_LOG_CATEGORY_EXTERN(LogModelContextProtocol, Log, All);

class FUnrealMCPServer : public TSharedFromThis<FUnrealMCPServer, ESPMode::ThreadSafe>
{
public:
    bool Start();
    void Stop();

private:
    struct FTaskState
    {
        FString Id;
        FString OwnerId;
        FString State = TEXT("running");
        FString CreatedAt;
        FString UpdatedAt;
        TSharedPtr<FJsonObject> Arguments;
        TArray<TSharedPtr<FJsonValue>> CommandResults;
        int32 NextCommandIndex = 0;
        double StartedAtSeconds = 0.0;
        bool bAllSucceeded = true;
        TSharedPtr<FJsonObject> Result;
        FString Error;
    };

    bool HandleMcpPost(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleMcpGet(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleMcpOptions(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    void ProcessRequestOnGameThread(FString Body, FString OwnerId, FHttpResultCallback OnComplete);

    TSharedRef<FJsonObject> ProcessJsonRpc(const TSharedRef<FJsonObject>& Request);
    TSharedRef<FJsonObject> InitializeResult(const TSharedRef<FJsonObject>& Request) const;
    TSharedRef<FJsonObject> ServerDiscoverResult() const;
    TSharedRef<FJsonObject> ToolsListResult() const;
    TSharedRef<FJsonObject> ToolsCallResult(const TSharedRef<FJsonObject>& Request);
    TSharedRef<FJsonObject> HandleAction(const TSharedRef<FJsonObject>& Arguments);
    TSharedRef<FJsonObject> Discover(const TSharedRef<FJsonObject>& Arguments) const;
    TSharedRef<FJsonObject> Health() const;
    TSharedRef<FJsonObject> ExecuteCommands(const TSharedRef<FJsonObject>& Arguments) const;
    TSharedRef<FJsonObject> ExecuteCommand(
        const TSharedPtr<FJsonValue>& CommandValue,
        int32 Index,
        bool bUseTransaction,
        bool& bSucceeded) const;
    TSharedRef<FJsonObject> MakeExecutionData(
        const TArray<TSharedPtr<FJsonValue>>& Results,
        bool bAllSucceeded,
        bool bUseTransaction,
        double StartedAtSeconds,
        bool bTimedOut = false) const;
    void RunNextTaskCommand(FString TaskId);
    void FinishTask(FTaskState& Task, const FString& State, const FString& Error = FString());
    TSharedRef<FJsonObject> HandleTask(const TSharedRef<FJsonObject>& Arguments);

    TSharedRef<FJsonObject> JsonRpcResult(
        const TSharedPtr<FJsonValue>& Id,
        const TSharedRef<FJsonObject>& Result) const;
    TSharedRef<FJsonObject> JsonRpcError(
        const TSharedPtr<FJsonValue>& Id,
        int32 Code,
        const FString& Message) const;
    TSharedRef<FJsonObject> ErrorPayload(const FString& Message) const;
    TSharedRef<FJsonObject> TaskSnapshot(const FTaskState& Task) const;
    void AddModernResultFields(const TSharedRef<FJsonObject>& Result) const;
    bool IsModernRequest(const TSharedRef<FJsonObject>& Request) const;
    bool LoadMetadata();
    bool IsAuthorized(const FHttpServerRequest& Request) const;
    bool IsOriginAllowed(const FHttpServerRequest& Request) const;
    FString RequestOwnerId(const FHttpServerRequest& Request) const;

    TUniquePtr<FHttpServerResponse> JsonResponse(
        const TSharedRef<FJsonObject>& Object,
        EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok) const;
    TUniquePtr<FHttpServerResponse> EmptyResponse(EHttpServerResponseCodes Code) const;
    TUniquePtr<FHttpServerResponse> HttpErrorResponse(
        EHttpServerResponseCodes Code,
        const FString& Error,
        const FString& Message) const;

    TSharedPtr<IHttpRouter> Router;
    FHttpRouteHandle McpPostRoute;
    FHttpRouteHandle McpGetRoute;
    FHttpRouteHandle McpOptionsRoute;
    TSharedPtr<FThreadSafeCounter, ESPMode::ThreadSafe> RequestQueueDepth;
    TSharedPtr<FJsonObject> Metadata;
    TMap<FString, FTaskState> Tasks;
    FString ActiveOwnerId;
    TSharedPtr<FThreadSafeBool, ESPMode::ThreadSafe> Lifetime;
    FString Token;
    FString EndpointPath = TEXT("/mcp");
    uint32 Port = 18777;
    bool bStarted = false;
};
