#include "UnrealMCPWorkerServer.h"

#include "Async/Async.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "IPythonScriptPlugin.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersion.h"
#include "Misc/OutputDevice.h"
#include "Misc/ScopeExit.h"
#include "Misc/StringOutputDevice.h"
#include "Modules/ModuleManager.h"
#include "PythonScriptTypes.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY(LogUnrealMCP)

namespace
{
    FString BodyToString(const TArray<uint8>& Body)
    {
        if (Body.IsEmpty())
        {
            return FString();
        }
        const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Body.GetData()), Body.Num());
        return FString(Converted.Length(), Converted.Get());
    }

    TSharedRef<FJsonObject> MakeCommandResult(
        int32 Index,
        const FString& Kind,
        const FString& Label,
        bool bSuccess)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetNumberField(TEXT("index"), Index);
        Result->SetStringField(TEXT("kind"), Kind);
        Result->SetBoolField(TEXT("ok"), bSuccess);
        if (!Label.IsEmpty())
        {
            Result->SetStringField(TEXT("label"), Label);
        }
        return Result;
    }
}

bool FUnrealMCPWorkerServer::Start()
{
    if (bStarted)
    {
        return true;
    }

    const FString PortValue = FPlatformMisc::GetEnvironmentVariable(TEXT("UE_MCP_WORKER_PORT"));
    if (!PortValue.IsEmpty())
    {
        const int32 ParsedPort = FCString::Atoi(*PortValue);
        if (ParsedPort > 0 && ParsedPort <= 65535)
        {
            Port = static_cast<uint32>(ParsedPort);
        }
        else
        {
            UE_LOG(LogUnrealMCP, Error, TEXT("Invalid UE_MCP_WORKER_PORT '%s'"), *PortValue);
            return false;
        }
    }
    Token = FPlatformMisc::GetEnvironmentVariable(TEXT("UE_MCP_WORKER_TOKEN"));

    // HTTPServer reads its bind address from GEngineIni. Insert an in-memory,
    // per-port override so this worker cannot inherit an unsafe wildcard bind.
    static const TCHAR* ListenerSection = TEXT("HTTPServer.Listeners");
    TArray<FString> ListenerOverrides;
    GConfig->GetArray(ListenerSection, TEXT("ListenerOverrides"), ListenerOverrides, GEngineIni);
    ListenerOverrides.RemoveAll([this](const FString& Existing)
    {
        uint32 ExistingPort = 0;
        return FParse::Value(*Existing, TEXT("Port="), ExistingPort) && ExistingPort == Port;
    });
    ListenerOverrides.Add(FString::Printf(TEXT("(Port=%u,BindAddress=localhost)"), Port));
    GConfig->SetArray(ListenerSection, TEXT("ListenerOverrides"), ListenerOverrides, GEngineIni);

    FHttpServerModule& HttpServer = FHttpServerModule::Get();
    Router = HttpServer.GetHttpRouter(Port, true);
    if (!Router)
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("Unable to bind loopback HTTP worker on port %u"), Port);
        return false;
    }

    HealthRoute = Router->BindRoute(
        FHttpPath(TEXT("/ue-mcp/v1/health")),
        EHttpServerRequestVerbs::VERB_GET,
        FHttpRequestHandler::CreateRaw(this, &FUnrealMCPWorkerServer::HandleHealth));
    ExecuteRoute = Router->BindRoute(
        FHttpPath(TEXT("/ue-mcp/v1/execute")),
        EHttpServerRequestVerbs::VERB_POST,
        FHttpRequestHandler::CreateRaw(this, &FUnrealMCPWorkerServer::HandleExecute));

    if (!HealthRoute || !ExecuteRoute)
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("Unable to register worker routes"));
        Stop();
        return false;
    }

    HttpServer.StartAllListeners();
    bStarted = true;
    UE_LOG(
        LogUnrealMCP,
        Log,
        TEXT("UE MCP worker listening on http://localhost:%u (token authentication: %s)"),
        Port,
        Token.IsEmpty() ? TEXT("disabled") : TEXT("enabled"));
    if (Token.IsEmpty())
    {
        UE_LOG(LogUnrealMCP, Warning, TEXT("Set UE_MCP_WORKER_TOKEN before launching the editor to require bearer authentication."));
    }
    return true;
}

void FUnrealMCPWorkerServer::Stop()
{
    if (Router)
    {
        if (HealthRoute)
        {
            Router->UnbindRoute(HealthRoute);
            HealthRoute.Reset();
        }
        if (ExecuteRoute)
        {
            Router->UnbindRoute(ExecuteRoute);
            ExecuteRoute.Reset();
        }
    }
    Router.Reset();
    bStarted = false;
}

bool FUnrealMCPWorkerServer::HandleHealth(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request))
    {
        OnComplete(ErrorResponse(EHttpServerResponseCodes::Denied, TEXT("unauthorized"), TEXT("Missing or invalid bearer token.")));
        return true;
    }

    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("ok"), true);
    Result->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
    Result->SetBoolField(TEXT("is_game_thread"), IsInGameThread());
    Result->SetBoolField(TEXT("python_loaded"), IPythonScriptPlugin::Get() != nullptr);
    Result->SetStringField(TEXT("transport"), TEXT("loopback-http"));
    OnComplete(JsonResponse(Result));
    return true;
}

bool FUnrealMCPWorkerServer::HandleExecute(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsAuthorized(Request))
    {
        OnComplete(ErrorResponse(EHttpServerResponseCodes::Denied, TEXT("unauthorized"), TEXT("Missing or invalid bearer token.")));
        return true;
    }
    if (Request.Body.Num() > 4 * 1024 * 1024)
    {
        OnComplete(ErrorResponse(EHttpServerResponseCodes::RequestTooLarge, TEXT("body_too_large"), TEXT("Request body exceeds 4 MiB.")));
        return true;
    }

    FString Body = BodyToString(Request.Body);
    if (Body.IsEmpty())
    {
        OnComplete(ErrorResponse(EHttpServerResponseCodes::BadRequest, TEXT("empty_body"), TEXT("Expected a JSON request body.")));
        return true;
    }

    if (IsInGameThread())
    {
        ExecuteOnGameThread(MoveTemp(Body), OnComplete);
    }
    else
    {
        AsyncTask(ENamedThreads::GameThread, [this, Body = MoveTemp(Body), OnComplete]() mutable
        {
            ExecuteOnGameThread(MoveTemp(Body), OnComplete);
        });
    }
    return true;
}

void FUnrealMCPWorkerServer::ExecuteOnGameThread(FString Body, FHttpResultCallback OnComplete)
{
    check(IsInGameThread());
    const double StartedAt = FPlatformTime::Seconds();

    TSharedPtr<FJsonObject> Request;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
    if (!FJsonSerializer::Deserialize(Reader, Request) || !Request)
    {
        OnComplete(ErrorResponse(EHttpServerResponseCodes::BadRequest, TEXT("invalid_json"), TEXT("Could not parse request JSON.")));
        return;
    }

    const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
    if (!Request->TryGetArrayField(TEXT("commands"), Commands) || !Commands || Commands->IsEmpty())
    {
        OnComplete(ErrorResponse(EHttpServerResponseCodes::BadRequest, TEXT("missing_commands"), TEXT("commands must be a non-empty array.")));
        return;
    }
    if (Commands->Num() > 100)
    {
        OnComplete(ErrorResponse(EHttpServerResponseCodes::BadRequest, TEXT("too_many_commands"), TEXT("A batch may contain at most 100 commands.")));
        return;
    }

    bool bUseTransaction = true;
    bool bContinueOnError = false;
    Request->TryGetBoolField(TEXT("transaction"), bUseTransaction);
    Request->TryGetBoolField(TEXT("continue_on_error"), bContinueOnError);

    TUniquePtr<FScopedTransaction> Transaction;
    if (bUseTransaction)
    {
        Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UnrealMCP", "BatchTransaction", "MCP command batch"));
    }

    bool bAllSucceeded = true;
    TArray<TSharedPtr<FJsonValue>> ResultValues;
    ResultValues.Reserve(Commands->Num());

    for (int32 Index = 0; Index < Commands->Num(); ++Index)
    {
        const TSharedPtr<FJsonObject>* CommandObject = nullptr;
        if (!(*Commands)[Index]->TryGetObject(CommandObject) || !CommandObject || !CommandObject->IsValid())
        {
            TSharedRef<FJsonObject> Result = MakeCommandResult(Index, TEXT("unknown"), FString(), false);
            Result->SetStringField(TEXT("error"), TEXT("Command must be a JSON object."));
            ResultValues.Add(MakeShared<FJsonValueObject>(Result));
            bAllSucceeded = false;
            if (!bContinueOnError) break;
            continue;
        }

        FString Kind;
        FString Label;
        (*CommandObject)->TryGetStringField(TEXT("kind"), Kind);
        (*CommandObject)->TryGetStringField(TEXT("label"), Label);

        if (Kind.Equals(TEXT("python"), ESearchCase::IgnoreCase))
        {
            FString Code;
            FString Mode = TEXT("exec");
            (*CommandObject)->TryGetStringField(TEXT("code"), Code);
            (*CommandObject)->TryGetStringField(TEXT("mode"), Mode);
            IPythonScriptPlugin* Python = FModuleManager::LoadModulePtr<IPythonScriptPlugin>(TEXT("PythonScriptPlugin"));
            if (Python && !Python->IsPythonInitialized())
            {
                Python->ForceEnablePythonAtRuntime();
            }

            FPythonCommandEx PythonCommand;
            PythonCommand.Flags = EPythonCommandFlags::Unattended;
            PythonCommand.FileExecutionScope = EPythonFileExecutionScope::Private;
            PythonCommand.ExecutionMode = Mode.Equals(TEXT("eval"), ESearchCase::IgnoreCase)
                ? EPythonCommandExecutionMode::EvaluateStatement
                : EPythonCommandExecutionMode::ExecuteFile;
            PythonCommand.Command = MoveTemp(Code);
            const bool bSuccess = Python && Python->IsPythonInitialized() && Python->ExecPythonCommandEx(PythonCommand);

            TSharedRef<FJsonObject> Result = MakeCommandResult(Index, TEXT("python"), Label, bSuccess);
            Result->SetStringField(TEXT("result"), PythonCommand.CommandResult);
            TArray<TSharedPtr<FJsonValue>> Logs;
            for (const FPythonLogOutputEntry& Log : PythonCommand.LogOutput)
            {
                TSharedRef<FJsonObject> LogObject = MakeShared<FJsonObject>();
                LogObject->SetStringField(TEXT("level"), LexToString(Log.Type));
                LogObject->SetStringField(TEXT("message"), Log.Output);
                Logs.Add(MakeShared<FJsonValueObject>(LogObject));
            }
            Result->SetArrayField(TEXT("logs"), MoveTemp(Logs));
            if (!Python)
            {
                Result->SetStringField(TEXT("error"), TEXT("PythonScriptPlugin is unavailable."));
            }
            ResultValues.Add(MakeShared<FJsonValueObject>(Result));
            bAllSucceeded &= bSuccess;
            if (!bSuccess && !bContinueOnError) break;
        }
        else if (Kind.Equals(TEXT("console"), ESearchCase::IgnoreCase))
        {
            FString Command;
            (*CommandObject)->TryGetStringField(TEXT("command"), Command);
            FStringOutputDevice Output;
            UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
            const bool bSuccess = GEngine && !Command.IsEmpty() && GEngine->Exec(World, *Command, Output);
            TSharedRef<FJsonObject> Result = MakeCommandResult(Index, TEXT("console"), Label, bSuccess);
            Result->SetStringField(TEXT("output"), Output);
            if (!bSuccess)
            {
                Result->SetStringField(TEXT("error"), TEXT("Console command was empty, unavailable, or unhandled."));
            }
            ResultValues.Add(MakeShared<FJsonValueObject>(Result));
            bAllSucceeded &= bSuccess;
            if (!bSuccess && !bContinueOnError) break;
        }
        else
        {
            TSharedRef<FJsonObject> Result = MakeCommandResult(Index, Kind, Label, false);
            Result->SetStringField(TEXT("error"), TEXT("Unsupported command kind. Expected 'python' or 'console'."));
            ResultValues.Add(MakeShared<FJsonValueObject>(Result));
            bAllSucceeded = false;
            if (!bContinueOnError) break;
        }
    }

    if (!bAllSucceeded && Transaction)
    {
        // Cancels the undo record; individual APIs may already have changed state.
        // Callers should use source control/preview queries for non-transactional asset operations.
        Transaction->Cancel();
    }

    TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("ok"), bAllSucceeded);
    Response->SetBoolField(TEXT("game_thread"), true);
    Response->SetBoolField(TEXT("transaction_recorded"), bAllSucceeded && bUseTransaction);
    Response->SetNumberField(TEXT("duration_ms"), (FPlatformTime::Seconds() - StartedAt) * 1000.0);
    Response->SetArrayField(TEXT("results"), MoveTemp(ResultValues));
    OnComplete(JsonResponse(Response, bAllSucceeded ? EHttpServerResponseCodes::Ok : EHttpServerResponseCodes::BadRequest));
}

bool FUnrealMCPWorkerServer::IsAuthorized(const FHttpServerRequest& Request) const
{
    if (Token.IsEmpty())
    {
        return true;
    }
    for (const TPair<FString, TArray<FString>>& Header : Request.Headers)
    {
        if (Header.Key.Equals(TEXT("Authorization"), ESearchCase::IgnoreCase))
        {
            return Header.Value.Contains(FString::Printf(TEXT("Bearer %s"), *Token));
        }
    }
    return false;
}

TUniquePtr<FHttpServerResponse> FUnrealMCPWorkerServer::JsonResponse(
    const TSharedRef<FJsonObject>& Object,
    EHttpServerResponseCodes Code) const
{
    FString Text;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
    FJsonSerializer::Serialize(Object, Writer);
    TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Text, TEXT("application/json; charset=utf-8"));
    Response->Code = Code;
    Response->Headers.Add(TEXT("Cache-Control"), { TEXT("no-store") });
    return Response;
}

TUniquePtr<FHttpServerResponse> FUnrealMCPWorkerServer::ErrorResponse(
    EHttpServerResponseCodes Code,
    const FString& Error,
    const FString& Message) const
{
    TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("ok"), false);
    Response->SetStringField(TEXT("error"), Error);
    Response->SetStringField(TEXT("message"), Message);
    return JsonResponse(Response, Code);
}
