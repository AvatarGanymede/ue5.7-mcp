#pragma once

#include "CoreMinimal.h"
#include "HttpRouteHandle.h"

class IHttpRouter;
struct FHttpServerRequest;
struct FHttpServerResponse;

DECLARE_LOG_CATEGORY_EXTERN(LogUnrealMCP, Log, All);

class FUnrealMCPWorkerServer
{
public:
    bool Start();
    void Stop();

private:
    bool HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    bool HandleExecute(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
    void ExecuteOnGameThread(FString Body, FHttpResultCallback OnComplete);

    bool IsAuthorized(const FHttpServerRequest& Request) const;
    TUniquePtr<FHttpServerResponse> JsonResponse(
        const TSharedRef<FJsonObject>& Object,
        EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok) const;
    TUniquePtr<FHttpServerResponse> ErrorResponse(
        EHttpServerResponseCodes Code,
        const FString& Error,
        const FString& Message) const;

    TSharedPtr<IHttpRouter> Router;
    FHttpRouteHandle HealthRoute;
    FHttpRouteHandle ExecuteRoute;
    FString Token;
    uint32 Port = 18777;
    bool bStarted = false;
};
