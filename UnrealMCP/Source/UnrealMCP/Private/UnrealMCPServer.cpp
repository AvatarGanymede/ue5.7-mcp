#include "UnrealMCPServer.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Interfaces/IPluginManager.h"
#include "IPythonScriptPlugin.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
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
    constexpr int32 MaxRequestBytes = 4 * 1024 * 1024;
    constexpr int32 MaxCommands = 100;
    constexpr int32 MaxTasks = 128;
    const TCHAR* LatestProtocolVersion = TEXT("2026-07-28");
    const TCHAR* ServerInstructions =
        TEXT("One tool is exposed. Use action=discover for unfamiliar Unreal APIs, then action=execute with a compact ")
        TEXT("Python/console batch. Use eval for queries, exec for mutations, and async plus task.get for long work. ")
        TEXT("UObject and editor calls run on Unreal's Game Thread.");

    FString ServerVersion()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealMCP"));
        return Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : TEXT("unknown");
    }

    FString BodyToString(const TArray<uint8>& Body)
    {
        if (Body.IsEmpty())
        {
            return FString();
        }
        const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Body.GetData()), Body.Num());
        return FString(Converted.Length(), Converted.Get());
    }

    FString JsonToString(const TSharedRef<FJsonObject>& Object, bool bPretty = false)
    {
        FString Text;
        if (bPretty)
        {
            const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
                TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Text);
            FJsonSerializer::Serialize(Object, Writer);
        }
        else
        {
            const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
                TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
            FJsonSerializer::Serialize(Object, Writer);
        }
        return Text;
    }

    FString JsonValueToString(const TSharedPtr<FJsonValue>& Value)
    {
        FString Text;
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
        FJsonSerializer::Serialize(Value.ToSharedRef(), FString(), Writer);
        return Text;
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

    FString UtcTimestamp()
    {
        return FDateTime::UtcNow().ToIso8601();
    }

    FString HeaderValue(const FHttpServerRequest& Request, const FString& Name)
    {
        for (const TPair<FString, TArray<FString>>& Header : Request.Headers)
        {
            if (Header.Key.Equals(Name, ESearchCase::IgnoreCase) && !Header.Value.IsEmpty())
            {
                return Header.Value[0];
            }
        }
        return FString();
    }
}

bool FUnrealMCPServer::Start()
{
    if (bStarted)
    {
        return true;
    }
    if (!LoadMetadata())
    {
        return false;
    }

    FString PortValue = FPlatformMisc::GetEnvironmentVariable(TEXT("UE_MCP_PORT"));
    if (PortValue.IsEmpty())
    {
        PortValue = FPlatformMisc::GetEnvironmentVariable(TEXT("UE_MCP_WORKER_PORT"));
    }
    if (!PortValue.IsEmpty())
    {
        const int32 ParsedPort = FCString::Atoi(*PortValue);
        if (ParsedPort <= 0 || ParsedPort > 65535)
        {
            UE_LOG(LogUnrealMCP, Error, TEXT("Invalid UE_MCP_PORT '%s'"), *PortValue);
            return false;
        }
        Port = static_cast<uint32>(ParsedPort);
    }

    Token = FPlatformMisc::GetEnvironmentVariable(TEXT("UE_MCP_TOKEN"));
    if (Token.IsEmpty())
    {
        Token = FPlatformMisc::GetEnvironmentVariable(TEXT("UE_MCP_WORKER_TOKEN"));
    }

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
        UE_LOG(LogUnrealMCP, Error, TEXT("Unable to bind MCP server on 127.0.0.1:%u"), Port);
        return false;
    }

    McpPostRoute = Router->BindRoute(
        FHttpPath(EndpointPath),
        EHttpServerRequestVerbs::VERB_POST,
        FHttpRequestHandler::CreateRaw(this, &FUnrealMCPServer::HandleMcpPost));
    McpGetRoute = Router->BindRoute(
        FHttpPath(EndpointPath),
        EHttpServerRequestVerbs::VERB_GET,
        FHttpRequestHandler::CreateRaw(this, &FUnrealMCPServer::HandleMcpGet));
    McpOptionsRoute = Router->BindRoute(
        FHttpPath(EndpointPath),
        EHttpServerRequestVerbs::VERB_OPTIONS,
        FHttpRequestHandler::CreateRaw(this, &FUnrealMCPServer::HandleMcpOptions));

    if (!McpPostRoute || !McpGetRoute || !McpOptionsRoute)
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("Unable to register MCP HTTP routes"));
        Stop();
        return false;
    }

    HttpServer.StartAllListeners();
    bStarted = true;
    UE_LOG(
        LogUnrealMCP,
        Log,
        TEXT("Unreal MCP listening on http://127.0.0.1:%u%s (Streamable HTTP, token authentication: %s)"),
        Port,
        *EndpointPath,
        Token.IsEmpty() ? TEXT("disabled") : TEXT("enabled"));
    if (Token.IsEmpty())
    {
        UE_LOG(LogUnrealMCP, Warning, TEXT("Set UE_MCP_TOKEN before launching the editor to require bearer authentication."));
    }
    return true;
}

void FUnrealMCPServer::Stop()
{
    if (Router)
    {
        if (McpPostRoute)
        {
            Router->UnbindRoute(McpPostRoute);
            McpPostRoute.Reset();
        }
        if (McpGetRoute)
        {
            Router->UnbindRoute(McpGetRoute);
            McpGetRoute.Reset();
        }
        if (McpOptionsRoute)
        {
            Router->UnbindRoute(McpOptionsRoute);
            McpOptionsRoute.Reset();
        }
    }
    Router.Reset();
    Tasks.Empty();
    Metadata.Reset();
    bStarted = false;
}

bool FUnrealMCPServer::HandleMcpPost(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsOriginAllowed(Request))
    {
        OnComplete(HttpErrorResponse(EHttpServerResponseCodes::Forbidden, TEXT("invalid_origin"), TEXT("Origin must be localhost.")));
        return true;
    }
    if (!IsAuthorized(Request))
    {
        TUniquePtr<FHttpServerResponse> Response =
            HttpErrorResponse(EHttpServerResponseCodes::Denied, TEXT("unauthorized"), TEXT("Missing or invalid bearer token."));
        Response->Headers.Add(TEXT("WWW-Authenticate"), { TEXT("Bearer") });
        OnComplete(MoveTemp(Response));
        return true;
    }
    if (Request.Body.Num() > MaxRequestBytes)
    {
        OnComplete(HttpErrorResponse(EHttpServerResponseCodes::RequestTooLarge, TEXT("body_too_large"), TEXT("Request body exceeds 4 MiB.")));
        return true;
    }

    FString Body = BodyToString(Request.Body);
    if (Body.IsEmpty())
    {
        OnComplete(HttpErrorResponse(EHttpServerResponseCodes::BadRequest, TEXT("empty_body"), TEXT("Expected a JSON-RPC request body.")));
        return true;
    }

    if (IsInGameThread())
    {
        ProcessRequestOnGameThread(MoveTemp(Body), OnComplete);
    }
    else
    {
        AsyncTask(ENamedThreads::GameThread, [this, Body = MoveTemp(Body), OnComplete]() mutable
        {
            ProcessRequestOnGameThread(MoveTemp(Body), OnComplete);
        });
    }
    return true;
}

bool FUnrealMCPServer::HandleMcpGet(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsOriginAllowed(Request))
    {
        OnComplete(HttpErrorResponse(EHttpServerResponseCodes::Forbidden, TEXT("invalid_origin"), TEXT("Origin must be localhost.")));
        return true;
    }
    if (!IsAuthorized(Request))
    {
        OnComplete(HttpErrorResponse(EHttpServerResponseCodes::Denied, TEXT("unauthorized"), TEXT("Missing or invalid bearer token.")));
        return true;
    }
    OnComplete(HttpErrorResponse(
        EHttpServerResponseCodes::BadMethod,
        TEXT("sse_not_supported"),
        TEXT("This stateless MCP server returns responses directly to POST requests and does not expose a standalone SSE stream.")));
    return true;
}

bool FUnrealMCPServer::HandleMcpOptions(const FHttpServerRequest&, const FHttpResultCallback& OnComplete)
{
    TUniquePtr<FHttpServerResponse> Response = EmptyResponse(EHttpServerResponseCodes::NoContent);
    Response->Headers.Add(TEXT("Allow"), { TEXT("POST, GET, OPTIONS") });
    OnComplete(MoveTemp(Response));
    return true;
}

void FUnrealMCPServer::ProcessRequestOnGameThread(FString Body, FHttpResultCallback OnComplete)
{
    check(IsInGameThread());
    TSharedPtr<FJsonObject> Request;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
    if (!FJsonSerializer::Deserialize(Reader, Request) || !Request)
    {
        OnComplete(JsonResponse(JsonRpcError(nullptr, -32700, TEXT("Parse error")), EHttpServerResponseCodes::BadRequest));
        return;
    }

    const TSharedPtr<FJsonValue> Id = Request->TryGetField(TEXT("id"));
    if (!Id || Id->IsNull())
    {
        // Streamable HTTP acknowledges JSON-RPC notifications without a body.
        OnComplete(EmptyResponse(EHttpServerResponseCodes::Accepted));
        return;
    }
    OnComplete(JsonResponse(ProcessJsonRpc(Request.ToSharedRef())));
}

TSharedRef<FJsonObject> FUnrealMCPServer::ProcessJsonRpc(const TSharedRef<FJsonObject>& Request)
{
    const TSharedPtr<FJsonValue> Id = Request->TryGetField(TEXT("id"));
    FString Method;
    if (!Request->TryGetStringField(TEXT("method"), Method))
    {
        return JsonRpcError(Id, -32600, TEXT("Invalid Request"));
    }

    if (Method == TEXT("server/discover"))
    {
        TSharedRef<FJsonObject> Result = ServerDiscoverResult();
        AddModernResultFields(Result);
        return JsonRpcResult(Id, Result);
    }
    if (Method == TEXT("initialize"))
    {
        return JsonRpcResult(Id, InitializeResult(Request));
    }
    if (Method == TEXT("ping"))
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        if (IsModernRequest(Request))
        {
            AddModernResultFields(Result);
        }
        return JsonRpcResult(Id, Result);
    }
    if (Method == TEXT("tools/list"))
    {
        TSharedRef<FJsonObject> Result = ToolsListResult();
        if (IsModernRequest(Request))
        {
            AddModernResultFields(Result);
        }
        return JsonRpcResult(Id, Result);
    }
    if (Method == TEXT("tools/call"))
    {
        TSharedRef<FJsonObject> Result = ToolsCallResult(Request);
        if (IsModernRequest(Request))
        {
            AddModernResultFields(Result);
        }
        return JsonRpcResult(Id, Result);
    }
    return JsonRpcError(Id, -32601, TEXT("Method not found"));
}

TSharedRef<FJsonObject> FUnrealMCPServer::InitializeResult(const TSharedRef<FJsonObject>& Request) const
{
    FString Protocol = TEXT("2025-11-25");
    const TSharedPtr<FJsonObject>* Params = nullptr;
    if (Request->TryGetObjectField(TEXT("params"), Params) && Params && Params->IsValid())
    {
        FString Requested;
        if ((*Params)->TryGetStringField(TEXT("protocolVersion"), Requested))
        {
            static const TSet<FString> Supported = {
                TEXT("2026-07-28"), TEXT("2025-11-25"), TEXT("2025-06-18"),
                TEXT("2025-03-26"), TEXT("2024-11-05") };
            if (Supported.Contains(Requested))
            {
                Protocol = Requested;
            }
        }
    }

    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("protocolVersion"), Protocol);
    TSharedRef<FJsonObject> Tools = MakeShared<FJsonObject>();
    Tools->SetBoolField(TEXT("listChanged"), false);
    TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
    Capabilities->SetObjectField(TEXT("tools"), Tools);
    Result->SetObjectField(TEXT("capabilities"), Capabilities);
    TSharedRef<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
    ServerInfo->SetStringField(TEXT("name"), TEXT("ue57-mcp"));
    ServerInfo->SetStringField(TEXT("title"), TEXT("Unreal Editor MCP"));
    ServerInfo->SetStringField(TEXT("version"), ServerVersion());
    Result->SetObjectField(TEXT("serverInfo"), ServerInfo);
    Result->SetStringField(TEXT("instructions"), ServerInstructions);
    return Result;
}

TSharedRef<FJsonObject> FUnrealMCPServer::ServerDiscoverResult() const
{
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("supportedVersions"), { MakeShared<FJsonValueString>(LatestProtocolVersion) });
    TSharedRef<FJsonObject> Tools = MakeShared<FJsonObject>();
    Tools->SetBoolField(TEXT("listChanged"), false);
    TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
    Capabilities->SetObjectField(TEXT("tools"), Tools);
    Result->SetObjectField(TEXT("capabilities"), Capabilities);
    Result->SetStringField(TEXT("instructions"), ServerInstructions);
    return Result;
}

TSharedRef<FJsonObject> FUnrealMCPServer::ToolsListResult() const
{
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("tools"), { Metadata->TryGetField(TEXT("tool")) });
    return Result;
}

TSharedRef<FJsonObject> FUnrealMCPServer::ToolsCallResult(const TSharedRef<FJsonObject>& Request)
{
    TSharedRef<FJsonObject> Payload = ErrorPayload(TEXT("tools/call requires params"));
    const TSharedPtr<FJsonObject>* Params = nullptr;
    if (Request->TryGetObjectField(TEXT("params"), Params) && Params && Params->IsValid())
    {
        FString Name;
        const TSharedPtr<FJsonObject>* Arguments = nullptr;
        if (!(*Params)->TryGetStringField(TEXT("name"), Name) || Name != TEXT("unreal"))
        {
            Payload = ErrorPayload(TEXT("Unknown tool; expected 'unreal'"));
        }
        else if (!(*Params)->TryGetObjectField(TEXT("arguments"), Arguments) || !Arguments || !Arguments->IsValid())
        {
            Payload = ErrorPayload(TEXT("unreal requires an arguments object"));
        }
        else
        {
            Payload = HandleAction(Arguments->ToSharedRef());
        }
    }

    bool bOk = false;
    Payload->TryGetBoolField(TEXT("ok"), bOk);
    TSharedRef<FJsonObject> TextContent = MakeShared<FJsonObject>();
    TextContent->SetStringField(TEXT("type"), TEXT("text"));
    TextContent->SetStringField(TEXT("text"), JsonToString(Payload, true));
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("content"), { MakeShared<FJsonValueObject>(TextContent) });
    Result->SetObjectField(TEXT("structuredContent"), Payload);
    Result->SetBoolField(TEXT("isError"), !bOk);
    return Result;
}

TSharedRef<FJsonObject> FUnrealMCPServer::HandleAction(const TSharedRef<FJsonObject>& Arguments)
{
    FString Action;
    if (!Arguments->TryGetStringField(TEXT("action"), Action))
    {
        return ErrorPayload(TEXT("action is required"));
    }
    if (Action == TEXT("discover"))
    {
        return Discover(Arguments);
    }
    if (Action == TEXT("health"))
    {
        return Health();
    }
    if (Action == TEXT("execute"))
    {
        const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
        if (!Arguments->TryGetArrayField(TEXT("commands"), Commands) || !Commands || Commands->IsEmpty())
        {
            return ErrorPayload(TEXT("execute requires a non-empty commands array"));
        }
        if (Commands->Num() > MaxCommands)
        {
            return ErrorPayload(TEXT("execute accepts at most 100 commands"));
        }

        FString Run;
        Arguments->TryGetStringField(TEXT("run"), Run);
        if (Run == TEXT("async"))
        {
            if (Tasks.Num() >= MaxTasks)
            {
                TArray<FString> CompletedIds;
                for (const TPair<FString, FTaskState>& Entry : Tasks)
                {
                    if (Entry.Value.State != TEXT("running"))
                    {
                        CompletedIds.Add(Entry.Key);
                    }
                }
                for (const FString& Id : CompletedIds)
                {
                    Tasks.Remove(Id);
                    if (Tasks.Num() < MaxTasks)
                    {
                        break;
                    }
                }
            }
            if (Tasks.Num() >= MaxTasks)
            {
                return ErrorPayload(TEXT("task store is full; wait for running tasks to finish"));
            }

            FTaskState Task;
            Task.Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
            Task.CreatedAt = UtcTimestamp();
            Task.UpdatedAt = Task.CreatedAt;
            const FString TaskId = Task.Id;
            Tasks.Add(TaskId, Task);

            AsyncTask(ENamedThreads::GameThread, [this, TaskId, Arguments]()
            {
                FTaskState* Existing = Tasks.Find(TaskId);
                if (!Existing || Existing->State == TEXT("cancelled"))
                {
                    return;
                }
                const TSharedRef<FJsonObject> Payload = ExecuteCommands(Arguments);
                Existing = Tasks.Find(TaskId);
                if (!Existing || Existing->State == TEXT("cancelled"))
                {
                    return;
                }
                bool bOk = false;
                Payload->TryGetBoolField(TEXT("ok"), bOk);
                Existing->UpdatedAt = UtcTimestamp();
                Existing->State = bOk ? TEXT("succeeded") : TEXT("failed");
                const TSharedPtr<FJsonObject>* Data = nullptr;
                if (Payload->TryGetObjectField(TEXT("data"), Data) && Data && Data->IsValid())
                {
                    Existing->Result = *Data;
                }
                if (!bOk)
                {
                    Payload->TryGetStringField(TEXT("error"), Existing->Error);
                }
            });

            TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
            Data->SetObjectField(TEXT("task"), TaskSnapshot(Task));
            TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetBoolField(TEXT("ok"), true);
            Payload->SetObjectField(TEXT("data"), Data);
            return Payload;
        }
        return ExecuteCommands(Arguments);
    }
    if (Action == TEXT("task"))
    {
        return HandleTask(Arguments);
    }
    return ErrorPayload(FString::Printf(TEXT("Unknown action '%s'"), *Action));
}

TSharedRef<FJsonObject> FUnrealMCPServer::Discover(const TSharedRef<FJsonObject>& Arguments) const
{
    FString Domain;
    FString Query;
    Arguments->TryGetStringField(TEXT("domain"), Domain);
    Arguments->TryGetStringField(TEXT("query"), Query);
    Domain = Domain.ToLower();

    FString Normalized = Query.ToLower();
    for (const TCHAR Character : FString(TEXT(",.;:/\\|()[]{}!?\"'")))
    {
        Normalized.ReplaceCharInline(Character, TEXT(' '));
    }
    TArray<FString> Terms;
    Normalized.ParseIntoArrayWS(Terms);

    int32 Limit = 12;
    Arguments->TryGetNumberField(TEXT("limit"), Limit);
    Limit = FMath::Clamp(Limit, 1, 50);

    struct FMatch
    {
        int32 Score = 0;
        TSharedPtr<FJsonValue> Capability;
    };
    TArray<FMatch> Matches;
    const TArray<TSharedPtr<FJsonValue>>& Capabilities = Metadata->GetArrayField(TEXT("capabilities"));
    for (const TSharedPtr<FJsonValue>& Value : Capabilities)
    {
        const TSharedPtr<FJsonObject> Capability = Value->AsObject();
        if (!Capability)
        {
            continue;
        }
        FString Id;
        if (!Capability->TryGetStringField(TEXT("id"), Id))
        {
            continue;
        }
        Id = Id.ToLower();
        if (!Domain.IsEmpty() && Id != Domain)
        {
            continue;
        }
        const FString Haystack = JsonValueToString(Value).ToLower();
        int32 Score = Domain == Id ? 1000 : 0;
        for (const FString& Term : Terms)
        {
            if (Id == Term)
            {
                Score += 50;
            }
            if (Haystack.Contains(Term))
            {
                Score += 10;
            }
        }
        if (Terms.IsEmpty() || Score > 0)
        {
            Matches.Add({ Score, Value });
        }
    }
    Matches.StableSort([](const FMatch& Left, const FMatch& Right)
    {
        return Left.Score > Right.Score;
    });

    TArray<TSharedPtr<FJsonValue>> Results;
    for (int32 Index = 0; Index < Matches.Num() && Index < Limit; ++Index)
    {
        Results.Add(Matches[Index].Capability);
    }
    TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("exposed_mcp_tools"), 1);
    Data->SetNumberField(TEXT("capability_domains"), Capabilities.Num());
    Data->SetArrayField(TEXT("official_all_toolsets_plugins"), Metadata->GetArrayField(TEXT("official_all_toolsets_plugins")));
    Data->SetArrayField(TEXT("results"), MoveTemp(Results));
    TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("ok"), true);
    Payload->SetObjectField(TEXT("data"), Data);
    return Payload;
}

TSharedRef<FJsonObject> FUnrealMCPServer::Health() const
{
    IPythonScriptPlugin* Python = FModuleManager::LoadModulePtr<IPythonScriptPlugin>(TEXT("PythonScriptPlugin"));
    TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("ok"), true);
    Data->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
    Data->SetBoolField(TEXT("is_game_thread"), IsInGameThread());
    Data->SetBoolField(TEXT("python_loaded"), Python && Python->IsPythonInitialized());
    Data->SetStringField(TEXT("transport"), TEXT("streamable-http"));
    Data->SetStringField(TEXT("endpoint"), FString::Printf(TEXT("http://127.0.0.1:%u%s"), Port, *EndpointPath));
    TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("ok"), true);
    Payload->SetObjectField(TEXT("data"), Data);
    return Payload;
}

TSharedRef<FJsonObject> FUnrealMCPServer::ExecuteCommands(const TSharedRef<FJsonObject>& Arguments) const
{
    check(IsInGameThread());
    const double StartedAt = FPlatformTime::Seconds();
    const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
    if (!Arguments->TryGetArrayField(TEXT("commands"), Commands) || !Commands || Commands->IsEmpty())
    {
        return ErrorPayload(TEXT("commands must be a non-empty array"));
    }

    bool bUseTransaction = true;
    bool bContinueOnError = false;
    Arguments->TryGetBoolField(TEXT("transaction"), bUseTransaction);
    Arguments->TryGetBoolField(TEXT("continue_on_error"), bContinueOnError);

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
        const TSharedPtr<FJsonObject> CommandObject = (*Commands)[Index]->AsObject();
        if (!CommandObject)
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
        CommandObject->TryGetStringField(TEXT("kind"), Kind);
        CommandObject->TryGetStringField(TEXT("label"), Label);
        if (Kind.Equals(TEXT("python"), ESearchCase::IgnoreCase))
        {
            FString Code;
            FString Mode = TEXT("exec");
            CommandObject->TryGetStringField(TEXT("code"), Code);
            CommandObject->TryGetStringField(TEXT("mode"), Mode);
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
            CommandObject->TryGetStringField(TEXT("command"), Command);
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
        Transaction->Cancel();
    }

    TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("ok"), bAllSucceeded);
    Data->SetBoolField(TEXT("game_thread"), true);
    Data->SetBoolField(TEXT("transaction_recorded"), bAllSucceeded && bUseTransaction);
    Data->SetNumberField(TEXT("duration_ms"), (FPlatformTime::Seconds() - StartedAt) * 1000.0);
    Data->SetArrayField(TEXT("results"), MoveTemp(ResultValues));
    TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("ok"), bAllSucceeded);
    Payload->SetObjectField(TEXT("data"), Data);
    if (!bAllSucceeded)
    {
        Payload->SetStringField(TEXT("error"), TEXT("One or more commands failed."));
    }
    return Payload;
}

TSharedRef<FJsonObject> FUnrealMCPServer::HandleTask(const TSharedRef<FJsonObject>& Arguments)
{
    FString Command;
    if (!Arguments->TryGetStringField(TEXT("command"), Command))
    {
        return ErrorPayload(TEXT("task.command is required"));
    }

    TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    if (Command == TEXT("list"))
    {
        TArray<FTaskState> Ordered;
        Tasks.GenerateValueArray(Ordered);
        Ordered.StableSort([](const FTaskState& Left, const FTaskState& Right)
        {
            return Left.CreatedAt > Right.CreatedAt;
        });
        TArray<TSharedPtr<FJsonValue>> Snapshots;
        for (const FTaskState& Task : Ordered)
        {
            Snapshots.Add(MakeShared<FJsonValueObject>(TaskSnapshot(Task)));
        }
        Data->SetArrayField(TEXT("tasks"), MoveTemp(Snapshots));
    }
    else
    {
        FString TaskId;
        if (!Arguments->TryGetStringField(TEXT("task_id"), TaskId))
        {
            return ErrorPayload(FString::Printf(TEXT("task_id is required for task.%s"), *Command));
        }
        FTaskState* Task = Tasks.Find(TaskId);
        if (!Task || (Command != TEXT("get") && Command != TEXT("cancel")))
        {
            return ErrorPayload(Task
                ? FString::Printf(TEXT("Unknown task command '%s'"), *Command)
                : FString::Printf(TEXT("Unknown task '%s'"), *TaskId));
        }
        if (Command == TEXT("cancel") && Task->State == TEXT("running"))
        {
            Task->State = TEXT("cancelled");
            Task->UpdatedAt = UtcTimestamp();
        }
        Data->SetObjectField(TEXT("task"), TaskSnapshot(*Task));
    }

    TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("ok"), true);
    Payload->SetObjectField(TEXT("data"), Data);
    return Payload;
}

TSharedRef<FJsonObject> FUnrealMCPServer::JsonRpcResult(
    const TSharedPtr<FJsonValue>& Id,
    const TSharedRef<FJsonObject>& Result) const
{
    TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
    Response->SetField(TEXT("id"), Id ? Id : MakeShared<FJsonValueNull>());
    Response->SetObjectField(TEXT("result"), Result);
    return Response;
}

TSharedRef<FJsonObject> FUnrealMCPServer::JsonRpcError(
    const TSharedPtr<FJsonValue>& Id,
    int32 Code,
    const FString& Message) const
{
    TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
    Error->SetNumberField(TEXT("code"), Code);
    Error->SetStringField(TEXT("message"), Message);
    TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
    Response->SetField(TEXT("id"), Id ? Id : MakeShared<FJsonValueNull>());
    Response->SetObjectField(TEXT("error"), Error);
    return Response;
}

TSharedRef<FJsonObject> FUnrealMCPServer::ErrorPayload(const FString& Message) const
{
    TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("ok"), false);
    Payload->SetStringField(TEXT("error"), Message);
    return Payload;
}

TSharedRef<FJsonObject> FUnrealMCPServer::TaskSnapshot(const FTaskState& Task) const
{
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("id"), Task.Id);
    Result->SetStringField(TEXT("state"), Task.State);
    Result->SetStringField(TEXT("created_at"), Task.CreatedAt);
    Result->SetStringField(TEXT("updated_at"), Task.UpdatedAt);
    if (Task.Result)
    {
        Result->SetObjectField(TEXT("result"), Task.Result.ToSharedRef());
    }
    if (!Task.Error.IsEmpty())
    {
        Result->SetStringField(TEXT("error"), Task.Error);
    }
    return Result;
}

void FUnrealMCPServer::AddModernResultFields(const TSharedRef<FJsonObject>& Result) const
{
    if (!Result->HasField(TEXT("resultType"))) Result->SetStringField(TEXT("resultType"), TEXT("complete"));
    if (!Result->HasField(TEXT("ttlMs"))) Result->SetNumberField(TEXT("ttlMs"), 0);
    if (!Result->HasField(TEXT("cacheScope"))) Result->SetStringField(TEXT("cacheScope"), TEXT("private"));
    if (!Result->HasField(TEXT("_meta")))
    {
        TSharedRef<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
        ServerInfo->SetStringField(TEXT("name"), TEXT("ue57-mcp"));
        ServerInfo->SetStringField(TEXT("title"), TEXT("Unreal Editor MCP"));
        ServerInfo->SetStringField(TEXT("version"), ServerVersion());
        TSharedRef<FJsonObject> Meta = MakeShared<FJsonObject>();
        Meta->SetObjectField(TEXT("io.modelcontextprotocol/serverInfo"), ServerInfo);
        Result->SetObjectField(TEXT("_meta"), Meta);
    }
}

bool FUnrealMCPServer::IsModernRequest(const TSharedRef<FJsonObject>& Request) const
{
    const TSharedPtr<FJsonObject>* Params = nullptr;
    const TSharedPtr<FJsonObject>* Meta = nullptr;
    FString Protocol;
    return Request->TryGetObjectField(TEXT("params"), Params) && Params && Params->IsValid()
        && (*Params)->TryGetObjectField(TEXT("_meta"), Meta) && Meta && Meta->IsValid()
        && (*Meta)->TryGetStringField(TEXT("io.modelcontextprotocol/protocolVersion"), Protocol)
        && Protocol >= LatestProtocolVersion;
}

bool FUnrealMCPServer::LoadMetadata()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealMCP"));
    if (!Plugin)
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("Unable to locate the UnrealMCP plugin directory."));
        return false;
    }
    const FString Path = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/UnrealMCP/metadata.json"));
    FString Text;
    if (!FFileHelper::LoadFileToString(Text, *Path))
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("Unable to read MCP metadata: %s"), *Path);
        return false;
    }
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
    if (!FJsonSerializer::Deserialize(Reader, Metadata) || !Metadata
        || !Metadata->HasTypedField<EJson::Object>(TEXT("tool"))
        || !Metadata->HasTypedField<EJson::Array>(TEXT("capabilities")))
    {
        UE_LOG(LogUnrealMCP, Error, TEXT("Invalid MCP metadata: %s"), *Path);
        Metadata.Reset();
        return false;
    }
    return true;
}

bool FUnrealMCPServer::IsAuthorized(const FHttpServerRequest& Request) const
{
    if (Token.IsEmpty())
    {
        return true;
    }
    return HeaderValue(Request, TEXT("Authorization")) == FString::Printf(TEXT("Bearer %s"), *Token);
}

bool FUnrealMCPServer::IsOriginAllowed(const FHttpServerRequest& Request) const
{
    const auto MatchesLocalAuthority = [](const FString& Value)
    {
        static const TArray<FString> Authorities = {
            TEXT("localhost"), TEXT("127.0.0.1"), TEXT("[::1]") };
        for (const FString& Authority : Authorities)
        {
            if (Value.Equals(Authority, ESearchCase::IgnoreCase)
                || Value.StartsWith(Authority + TEXT(":"), ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    };

    const FString Host = HeaderValue(Request, TEXT("Host"));
    if (!Host.IsEmpty() && !MatchesLocalAuthority(Host))
    {
        return false;
    }

    const FString Origin = HeaderValue(Request, TEXT("Origin"));
    if (Origin.IsEmpty())
    {
        return true;
    }
    FString Authority = Origin;
    if (Authority.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase))
    {
        Authority.RightChopInline(7);
    }
    else if (Authority.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase))
    {
        Authority.RightChopInline(8);
    }
    else
    {
        return false;
    }
    return !Authority.Contains(TEXT("/")) && MatchesLocalAuthority(Authority);
}

TUniquePtr<FHttpServerResponse> FUnrealMCPServer::JsonResponse(
    const TSharedRef<FJsonObject>& Object,
    EHttpServerResponseCodes Code) const
{
    TUniquePtr<FHttpServerResponse> Response =
        FHttpServerResponse::Create(JsonToString(Object), TEXT("application/json; charset=utf-8"));
    Response->Code = Code;
    Response->Headers.Add(TEXT("Cache-Control"), { TEXT("no-store") });
    return Response;
}

TUniquePtr<FHttpServerResponse> FUnrealMCPServer::EmptyResponse(EHttpServerResponseCodes Code) const
{
    TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(FString(), TEXT("text/plain; charset=utf-8"));
    Response->Code = Code;
    Response->Headers.Add(TEXT("Cache-Control"), { TEXT("no-store") });
    return Response;
}

TUniquePtr<FHttpServerResponse> FUnrealMCPServer::HttpErrorResponse(
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
