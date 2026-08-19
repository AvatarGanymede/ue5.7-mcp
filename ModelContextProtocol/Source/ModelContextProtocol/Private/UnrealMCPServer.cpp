#include "UnrealMCPServer.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
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

DEFINE_LOG_CATEGORY(LogModelContextProtocol)

namespace
{
    constexpr int32 MaxRequestBytes = 4 * 1024 * 1024;
    constexpr int32 MaxResponseBytes = 4 * 1024 * 1024;
    constexpr int32 MaxQueuedRequests = 64;
    constexpr int32 MaxCommands = 100;
    constexpr int32 MaxTasksPerOwner = 128;
    constexpr int32 MaxTasks = 1024;
    constexpr double MaxExecutionSeconds = 300.0;
    const TCHAR* LatestProtocolVersion = TEXT("2026-07-28");
    const TCHAR* ServerInstructions =
        TEXT("One tool is exposed. Use action=discover for unfamiliar Unreal APIs, then action=execute with a compact ")
        TEXT("Python/console batch. Use eval for queries, exec for mutations, and async plus task.get for long work. ")
        TEXT("UObject and editor calls run on Unreal's Game Thread.");

    FString ServerVersion()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ModelContextProtocol"));
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

    bool MatchesSchemaType(const TSharedPtr<FJsonValue>& Value, const FString& Type)
    {
        if (!Value)
        {
            return false;
        }
        if (Type == TEXT("null")) return Value->IsNull();
        if (Type == TEXT("object")) return Value->Type == EJson::Object;
        if (Type == TEXT("array")) return Value->Type == EJson::Array;
        if (Type == TEXT("string")) return Value->Type == EJson::String;
        if (Type == TEXT("boolean")) return Value->Type == EJson::Boolean;
        if (Type == TEXT("number")) return Value->Type == EJson::Number;
        if (Type == TEXT("integer"))
        {
            return Value->Type == EJson::Number
                && Value->AsNumber() == FMath::TruncToDouble(Value->AsNumber());
        }
        return false;
    }

    bool ValidateJsonSchema(
        const TSharedPtr<FJsonValue>& Value,
        const TSharedPtr<FJsonObject>& Schema,
        const FString& Path,
        FString& OutError)
    {
        if (!Value || !Schema)
        {
            OutError = FString::Printf(TEXT("%s has no value or schema"), *Path);
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* OneOf = nullptr;
        if (Schema->TryGetArrayField(TEXT("oneOf"), OneOf) && OneOf)
        {
            int32 Matches = 0;
            FString FirstError;
            for (const TSharedPtr<FJsonValue>& Candidate : *OneOf)
            {
                const TSharedPtr<FJsonObject> CandidateSchema = Candidate ? Candidate->AsObject() : nullptr;
                FString CandidateError;
                if (CandidateSchema && ValidateJsonSchema(Value, CandidateSchema, Path, CandidateError))
                {
                    ++Matches;
                }
                else if (FirstError.IsEmpty())
                {
                    FirstError = MoveTemp(CandidateError);
                }
            }
            if (Matches != 1)
            {
                OutError = Matches == 0 && !FirstError.IsEmpty()
                    ? MoveTemp(FirstError)
                    : FString::Printf(TEXT("%s must match exactly one schema branch (matched %d)"), *Path, Matches);
                return false;
            }
        }

        FString Type;
        if (Schema->TryGetStringField(TEXT("type"), Type) && !MatchesSchemaType(Value, Type))
        {
            OutError = FString::Printf(TEXT("%s must be of type %s"), *Path, *Type);
            return false;
        }

        const TSharedPtr<FJsonValue> ConstValue = Schema->TryGetField(TEXT("const"));
        if (ConstValue && !FJsonValue::CompareEqual(*Value, *ConstValue))
        {
            OutError = FString::Printf(TEXT("%s does not match its required constant"), *Path);
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* EnumValues = nullptr;
        if (Schema->TryGetArrayField(TEXT("enum"), EnumValues) && EnumValues)
        {
            const bool bMatchesEnum = EnumValues->ContainsByPredicate([&Value](const TSharedPtr<FJsonValue>& Candidate)
            {
                return Candidate && FJsonValue::CompareEqual(*Value, *Candidate);
            });
            if (!bMatchesEnum)
            {
                OutError = FString::Printf(TEXT("%s is not an allowed value"), *Path);
                return false;
            }
        }

        if (Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> Object = Value->AsObject();
            const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
            if (Schema->TryGetArrayField(TEXT("required"), Required) && Required)
            {
                for (const TSharedPtr<FJsonValue>& RequiredValue : *Required)
                {
                    FString RequiredName;
                    if (RequiredValue && RequiredValue->TryGetString(RequiredName) && !Object->HasField(RequiredName))
                    {
                        OutError = FString::Printf(TEXT("%s.%s is required"), *Path, *RequiredName);
                        return false;
                    }
                }
            }

            const TSharedPtr<FJsonObject>* Properties = nullptr;
            Schema->TryGetObjectField(TEXT("properties"), Properties);
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
            {
                const TSharedPtr<FJsonValue> PropertySchemaValue =
                    Properties && Properties->IsValid() ? (*Properties)->TryGetField(Field.Key) : nullptr;
                if (PropertySchemaValue)
                {
                    const TSharedPtr<FJsonObject> PropertySchema = PropertySchemaValue->AsObject();
                    if (!PropertySchema || !ValidateJsonSchema(
                        Field.Value, PropertySchema, Path + TEXT(".") + Field.Key, OutError))
                    {
                        return false;
                    }
                }
                else
                {
                    bool bAdditionalProperties = true;
                    if (Schema->TryGetBoolField(TEXT("additionalProperties"), bAdditionalProperties)
                        && !bAdditionalProperties)
                    {
                        OutError = FString::Printf(TEXT("%s.%s is not allowed"), *Path, *Field.Key);
                        return false;
                    }
                }
            }
        }

        if (Value->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
            int32 Minimum = 0;
            int32 Maximum = 0;
            if (Schema->TryGetNumberField(TEXT("minItems"), Minimum) && Values.Num() < Minimum)
            {
                OutError = FString::Printf(TEXT("%s must contain at least %d items"), *Path, Minimum);
                return false;
            }
            if (Schema->TryGetNumberField(TEXT("maxItems"), Maximum) && Values.Num() > Maximum)
            {
                OutError = FString::Printf(TEXT("%s must contain at most %d items"), *Path, Maximum);
                return false;
            }
            const TSharedPtr<FJsonObject>* ItemSchema = nullptr;
            if (Schema->TryGetObjectField(TEXT("items"), ItemSchema) && ItemSchema && ItemSchema->IsValid())
            {
                for (int32 Index = 0; Index < Values.Num(); ++Index)
                {
                    if (!ValidateJsonSchema(
                        Values[Index], *ItemSchema, FString::Printf(TEXT("%s[%d]"), *Path, Index), OutError))
                    {
                        return false;
                    }
                }
            }
        }

        if (Value->Type == EJson::String)
        {
            const FString StringValue = Value->AsString();
            int32 Minimum = 0;
            int32 Maximum = 0;
            if (Schema->TryGetNumberField(TEXT("minLength"), Minimum) && StringValue.Len() < Minimum)
            {
                OutError = FString::Printf(TEXT("%s must contain at least %d characters"), *Path, Minimum);
                return false;
            }
            if (Schema->TryGetNumberField(TEXT("maxLength"), Maximum) && StringValue.Len() > Maximum)
            {
                OutError = FString::Printf(TEXT("%s must contain at most %d characters"), *Path, Maximum);
                return false;
            }
            FString Format;
            if (Schema->TryGetStringField(TEXT("format"), Format) && Format == TEXT("uuid"))
            {
                FGuid Guid;
                if (!FGuid::Parse(StringValue, Guid)
                    || !Guid.ToString(EGuidFormats::DigitsWithHyphensLower).Equals(StringValue, ESearchCase::IgnoreCase))
                {
                    OutError = FString::Printf(TEXT("%s must be a UUID"), *Path);
                    return false;
                }
            }
        }

        if (Value->Type == EJson::Number)
        {
            double Minimum = 0.0;
            double Maximum = 0.0;
            if (Schema->TryGetNumberField(TEXT("minimum"), Minimum) && Value->AsNumber() < Minimum)
            {
                OutError = FString::Printf(TEXT("%s must be at least %g"), *Path, Minimum);
                return false;
            }
            if (Schema->TryGetNumberField(TEXT("maximum"), Maximum) && Value->AsNumber() > Maximum)
            {
                OutError = FString::Printf(TEXT("%s must be at most %g"), *Path, Maximum);
                return false;
            }
        }
        return true;
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
    Lifetime = MakeShared<FThreadSafeBool, ESPMode::ThreadSafe>(true);
    RequestQueueDepth = MakeShared<FThreadSafeCounter, ESPMode::ThreadSafe>();

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
            UE_LOG(LogModelContextProtocol, Error, TEXT("Invalid UE_MCP_PORT '%s'"), *PortValue);
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
        UE_LOG(LogModelContextProtocol, Error, TEXT("Unable to bind MCP server on 127.0.0.1:%u"), Port);
        Stop();
        return false;
    }

    McpPostRoute = Router->BindRoute(
        FHttpPath(EndpointPath),
        EHttpServerRequestVerbs::VERB_POST,
        FHttpRequestHandler::CreateSP(AsShared(), &FUnrealMCPServer::HandleMcpPost));
    McpGetRoute = Router->BindRoute(
        FHttpPath(EndpointPath),
        EHttpServerRequestVerbs::VERB_GET,
        FHttpRequestHandler::CreateSP(AsShared(), &FUnrealMCPServer::HandleMcpGet));
    McpOptionsRoute = Router->BindRoute(
        FHttpPath(EndpointPath),
        EHttpServerRequestVerbs::VERB_OPTIONS,
        FHttpRequestHandler::CreateSP(AsShared(), &FUnrealMCPServer::HandleMcpOptions));

    if (!McpPostRoute || !McpGetRoute || !McpOptionsRoute)
    {
        UE_LOG(LogModelContextProtocol, Error, TEXT("Unable to register MCP HTTP routes"));
        Stop();
        return false;
    }

    HttpServer.StartAllListeners();
    bStarted = true;
    UE_LOG(
        LogModelContextProtocol,
        Log,
        TEXT("Unreal MCP listening on http://127.0.0.1:%u%s (Streamable HTTP, token authentication: %s)"),
        Port,
        *EndpointPath,
        Token.IsEmpty() ? TEXT("disabled") : TEXT("enabled"));
    if (Token.IsEmpty())
    {
        UE_LOG(LogModelContextProtocol, Warning, TEXT("Set UE_MCP_TOKEN before launching the editor to require bearer authentication."));
    }
    return true;
}

void FUnrealMCPServer::Stop()
{
    if (Lifetime)
    {
        Lifetime->AtomicSet(false);
    }
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
    ActiveOwnerId.Reset();
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

    FString OwnerId = RequestOwnerId(Request);
    if (IsInGameThread())
    {
        ProcessRequestOnGameThread(MoveTemp(Body), MoveTemp(OwnerId), OnComplete);
    }
    else
    {
        const TSharedPtr<FThreadSafeCounter, ESPMode::ThreadSafe> QueueDepth = RequestQueueDepth;
        if (!QueueDepth || QueueDepth->Increment() > MaxQueuedRequests)
        {
            if (QueueDepth)
            {
                QueueDepth->Decrement();
            }
            TUniquePtr<FHttpServerResponse> Response = HttpErrorResponse(
                EHttpServerResponseCodes::TooManyRequests,
                TEXT("request_queue_full"),
                TEXT("The MCP request queue is full; retry later."));
            Response->Headers.Add(TEXT("Retry-After"), { TEXT("1") });
            OnComplete(MoveTemp(Response));
            return true;
        }
        const TSharedPtr<FThreadSafeBool, ESPMode::ThreadSafe> RequestLifetime = Lifetime;
        AsyncTask(ENamedThreads::GameThread, [this, RequestLifetime, QueueDepth, Body = MoveTemp(Body), OwnerId = MoveTemp(OwnerId), OnComplete]() mutable
        {
            QueueDepth->Decrement();
            if (!RequestLifetime || !static_cast<bool>(*RequestLifetime))
            {
                TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(
                    TEXT("{\"ok\":false,\"error\":\"server_stopping\"}"),
                    TEXT("application/json; charset=utf-8"));
                Response->Code = EHttpServerResponseCodes::BadRequest;
                OnComplete(MoveTemp(Response));
                return;
            }
            ProcessRequestOnGameThread(MoveTemp(Body), MoveTemp(OwnerId), OnComplete);
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
        TUniquePtr<FHttpServerResponse> Response =
            HttpErrorResponse(EHttpServerResponseCodes::Denied, TEXT("unauthorized"), TEXT("Missing or invalid bearer token."));
        Response->Headers.Add(TEXT("WWW-Authenticate"), { TEXT("Bearer") });
        OnComplete(MoveTemp(Response));
        return true;
    }
    OnComplete(HttpErrorResponse(
        EHttpServerResponseCodes::BadMethod,
        TEXT("sse_not_supported"),
        TEXT("This stateless MCP server returns responses directly to POST requests and does not expose a standalone SSE stream.")));
    return true;
}

bool FUnrealMCPServer::HandleMcpOptions(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
    if (!IsOriginAllowed(Request))
    {
        OnComplete(HttpErrorResponse(EHttpServerResponseCodes::Forbidden, TEXT("invalid_origin"), TEXT("Origin must be localhost.")));
        return true;
    }
    TUniquePtr<FHttpServerResponse> Response = EmptyResponse(EHttpServerResponseCodes::NoContent);
    Response->Headers.Add(TEXT("Allow"), { TEXT("POST, GET, OPTIONS") });
    OnComplete(MoveTemp(Response));
    return true;
}

void FUnrealMCPServer::ProcessRequestOnGameThread(FString Body, FString OwnerId, FHttpResultCallback OnComplete)
{
    check(IsInGameThread());
    TSharedPtr<FJsonObject> Request;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
    if (!FJsonSerializer::Deserialize(Reader, Request) || !Request)
    {
        TUniquePtr<FHttpServerResponse> Response =
            JsonResponse(JsonRpcError(nullptr, -32700, TEXT("Parse error")), EHttpServerResponseCodes::BadRequest);
        Response->Headers.Add(TEXT("Mcp-Session-Id"), { OwnerId });
        OnComplete(MoveTemp(Response));
        return;
    }

    const TSharedPtr<FJsonValue> Id = Request->TryGetField(TEXT("id"));
    if (!Id || Id->IsNull())
    {
        // Streamable HTTP acknowledges JSON-RPC notifications without a body.
        TUniquePtr<FHttpServerResponse> Response = EmptyResponse(EHttpServerResponseCodes::Accepted);
        Response->Headers.Add(TEXT("Mcp-Session-Id"), { OwnerId });
        OnComplete(MoveTemp(Response));
        return;
    }
    ActiveOwnerId = OwnerId;
    TUniquePtr<FHttpServerResponse> Response = JsonResponse(ProcessJsonRpc(Request.ToSharedRef()));
    ActiveOwnerId.Reset();
    Response->Headers.Add(TEXT("Mcp-Session-Id"), { OwnerId });
    OnComplete(MoveTemp(Response));
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
            const TSharedPtr<FJsonObject> Tool = Metadata->GetObjectField(TEXT("tool"));
            const TSharedPtr<FJsonObject> InputSchema = Tool->GetObjectField(TEXT("inputSchema"));
            FString ValidationError;
            if (!ValidateJsonSchema(
                MakeShared<FJsonValueObject>(*Arguments), InputSchema, TEXT("arguments"), ValidationError))
            {
                Payload = ErrorPayload(TEXT("Invalid arguments: ") + ValidationError);
            }
            else
            {
                Payload = HandleAction(Arguments->ToSharedRef());
            }
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
                return ErrorPayload(TEXT("global task store is full; wait for running tasks to finish"));
            }

            int32 OwnerTaskCount = 0;
            for (const TPair<FString, FTaskState>& Entry : Tasks)
            {
                OwnerTaskCount += Entry.Value.OwnerId == ActiveOwnerId ? 1 : 0;
            }
            if (OwnerTaskCount >= MaxTasksPerOwner)
            {
                TArray<FString> CompletedIds;
                for (const TPair<FString, FTaskState>& Entry : Tasks)
                {
                    if (Entry.Value.OwnerId == ActiveOwnerId && Entry.Value.State != TEXT("running"))
                    {
                        CompletedIds.Add(Entry.Key);
                    }
                }
                for (const FString& Id : CompletedIds)
                {
                    Tasks.Remove(Id);
                    if (--OwnerTaskCount < MaxTasksPerOwner)
                    {
                        break;
                    }
                }
            }
            if (OwnerTaskCount >= MaxTasksPerOwner)
            {
                return ErrorPayload(TEXT("task store is full for this MCP session; wait for running tasks to finish"));
            }

            FTaskState Task;
            Task.Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
            Task.OwnerId = ActiveOwnerId;
            Task.CreatedAt = UtcTimestamp();
            Task.UpdatedAt = Task.CreatedAt;
            Task.Arguments = Arguments;
            Task.StartedAtSeconds = FPlatformTime::Seconds();
            const FString TaskId = Task.Id;
            Tasks.Add(TaskId, Task);

            const TSharedPtr<FThreadSafeBool, ESPMode::ThreadSafe> TaskLifetime = Lifetime;
            AsyncTask(ENamedThreads::GameThread, [this, TaskLifetime, TaskId]()
            {
                if (!TaskLifetime || !static_cast<bool>(*TaskLifetime))
                {
                    return;
                }
                RunNextTaskCommand(TaskId);
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

    bool bAllSucceeded = true;
    bool bTimedOut = false;
    TArray<TSharedPtr<FJsonValue>> ResultValues;
    ResultValues.Reserve(Commands->Num());
    for (int32 Index = 0; Index < Commands->Num(); ++Index)
    {
        if ((FPlatformTime::Seconds() - StartedAt) >= MaxExecutionSeconds)
        {
            TSharedRef<FJsonObject> Result = MakeCommandResult(Index, TEXT("unknown"), FString(), false);
            Result->SetStringField(TEXT("error"), TEXT("Execution time limit exceeded (300 seconds)."));
            ResultValues.Add(MakeShared<FJsonValueObject>(Result));
            bAllSucceeded = false;
            bTimedOut = true;
            break;
        }

        bool bSucceeded = false;
        ResultValues.Add(MakeShared<FJsonValueObject>(
            ExecuteCommand((*Commands)[Index], Index, bUseTransaction, bSucceeded)));
        bAllSucceeded &= bSucceeded;
        if ((FPlatformTime::Seconds() - StartedAt) >= MaxExecutionSeconds)
        {
            bTimedOut = true;
            bAllSucceeded = false;
            break;
        }
        if (!bSucceeded && !bContinueOnError)
        {
            break;
        }
    }

    TSharedRef<FJsonObject> Data = MakeExecutionData(
        ResultValues, bAllSucceeded, bUseTransaction, StartedAt, bTimedOut);
    TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("ok"), bAllSucceeded);
    Payload->SetObjectField(TEXT("data"), Data);
    if (!bAllSucceeded)
    {
        Payload->SetStringField(TEXT("error"), bTimedOut
            ? TEXT("Execution time limit exceeded (300 seconds).")
            : TEXT("One or more commands failed."));
    }
    return Payload;
}

TSharedRef<FJsonObject> FUnrealMCPServer::ExecuteCommand(
    const TSharedPtr<FJsonValue>& CommandValue,
    int32 Index,
    bool bUseTransaction,
    bool& bSucceeded) const
{
    check(IsInGameThread());
    bSucceeded = false;
    const TSharedPtr<FJsonObject> CommandObject = CommandValue ? CommandValue->AsObject() : nullptr;
    if (!CommandObject)
    {
        TSharedRef<FJsonObject> Result = MakeCommandResult(Index, TEXT("unknown"), FString(), false);
        Result->SetStringField(TEXT("error"), TEXT("Command must be a JSON object."));
        return Result;
    }

    FString Kind;
    FString Label;
    CommandObject->TryGetStringField(TEXT("kind"), Kind);
    CommandObject->TryGetStringField(TEXT("label"), Label);

    // transaction=true records undo per command. It is intentionally not an
    // atomic batch: later failure or cancellation never rolls back earlier work.
    TUniquePtr<FScopedTransaction> Transaction;
    if (bUseTransaction)
    {
        Transaction = MakeUnique<FScopedTransaction>(FText::Format(
            NSLOCTEXT("ModelContextProtocol", "CommandTransaction", "MCP command {0}"),
            FText::AsNumber(Index + 1)));
    }

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
        bSucceeded = Python && Python->IsPythonInitialized() && Python->ExecPythonCommandEx(PythonCommand);

        TSharedRef<FJsonObject> Result = MakeCommandResult(Index, TEXT("python"), Label, bSucceeded);
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
        return Result;
    }

    if (Kind.Equals(TEXT("console"), ESearchCase::IgnoreCase))
    {
        FString Command;
        CommandObject->TryGetStringField(TEXT("command"), Command);
        FStringOutputDevice Output;
        UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        bSucceeded = GEngine && !Command.IsEmpty() && GEngine->Exec(World, *Command, Output);
        TSharedRef<FJsonObject> Result = MakeCommandResult(Index, TEXT("console"), Label, bSucceeded);
        Result->SetStringField(TEXT("output"), Output);
        if (!bSucceeded)
        {
            Result->SetStringField(TEXT("error"), TEXT("Console command was empty, unavailable, or unhandled."));
        }
        return Result;
    }

    TSharedRef<FJsonObject> Result = MakeCommandResult(Index, Kind, Label, false);
    Result->SetStringField(TEXT("error"), TEXT("Unsupported command kind. Expected 'python' or 'console'."));
    return Result;
}

TSharedRef<FJsonObject> FUnrealMCPServer::MakeExecutionData(
    const TArray<TSharedPtr<FJsonValue>>& Results,
    bool bAllSucceeded,
    bool bUseTransaction,
    double StartedAtSeconds,
    bool bTimedOut) const
{
    TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("ok"), bAllSucceeded);
    Data->SetBoolField(TEXT("game_thread"), true);
    Data->SetBoolField(TEXT("transaction_recorded"), bUseTransaction && !Results.IsEmpty());
    Data->SetBoolField(TEXT("transaction_atomic"), false);
    Data->SetBoolField(TEXT("transaction_rolled_back"), false);
    Data->SetBoolField(TEXT("timed_out"), bTimedOut);
    Data->SetNumberField(TEXT("duration_ms"), (FPlatformTime::Seconds() - StartedAtSeconds) * 1000.0);
    Data->SetArrayField(TEXT("results"), Results);
    return Data;
}

void FUnrealMCPServer::RunNextTaskCommand(FString TaskId)
{
    check(IsInGameThread());
    FTaskState* Task = Tasks.Find(TaskId);
    if (!Task || Task->State != TEXT("running") || !Task->Arguments)
    {
        return;
    }

    const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
    if (!Task->Arguments->TryGetArrayField(TEXT("commands"), Commands) || !Commands)
    {
        FinishTask(*Task, TEXT("failed"), TEXT("commands must be a non-empty array"));
        return;
    }
    if ((FPlatformTime::Seconds() - Task->StartedAtSeconds) >= MaxExecutionSeconds)
    {
        FinishTask(*Task, TEXT("timed_out"), TEXT("Execution time limit exceeded (300 seconds)."));
        return;
    }
    if (Task->NextCommandIndex >= Commands->Num())
    {
        FinishTask(*Task, Task->bAllSucceeded ? TEXT("succeeded") : TEXT("failed"),
            Task->bAllSucceeded ? FString() : TEXT("One or more commands failed."));
        return;
    }

    bool bUseTransaction = true;
    bool bContinueOnError = false;
    Task->Arguments->TryGetBoolField(TEXT("transaction"), bUseTransaction);
    Task->Arguments->TryGetBoolField(TEXT("continue_on_error"), bContinueOnError);

    const int32 CommandIndex = Task->NextCommandIndex++;
    bool bSucceeded = false;
    Task->CommandResults.Add(MakeShared<FJsonValueObject>(
        ExecuteCommand((*Commands)[CommandIndex], CommandIndex, bUseTransaction, bSucceeded)));
    Task->bAllSucceeded &= bSucceeded;
    Task->UpdatedAt = UtcTimestamp();
    if (!bSucceeded && !bContinueOnError)
    {
        FinishTask(*Task, TEXT("failed"), TEXT("One or more commands failed."));
        return;
    }

    // Yield after every command. Cancellation and timeout are observed only at
    // the next command boundary; an in-flight command is never interrupted.
    const TSharedPtr<FThreadSafeBool, ESPMode::ThreadSafe> TaskLifetime = Lifetime;
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
        [this, TaskLifetime, TaskId](float)
        {
            if (TaskLifetime && static_cast<bool>(*TaskLifetime))
            {
                RunNextTaskCommand(TaskId);
            }
            return false;
        }));
}

void FUnrealMCPServer::FinishTask(FTaskState& Task, const FString& State, const FString& Error)
{
    bool bUseTransaction = true;
    if (Task.Arguments)
    {
        Task.Arguments->TryGetBoolField(TEXT("transaction"), bUseTransaction);
    }
    Task.State = State;
    Task.UpdatedAt = UtcTimestamp();
    Task.Error = Error;
    Task.Result = MakeExecutionData(
        Task.CommandResults,
        State == TEXT("succeeded"),
        bUseTransaction,
        Task.StartedAtSeconds,
        Error == TEXT("Execution time limit exceeded (300 seconds)."));
    Task.Arguments.Reset();
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
        for (const TPair<FString, FTaskState>& Entry : Tasks)
        {
            if (Entry.Value.OwnerId == ActiveOwnerId)
            {
                Ordered.Add(Entry.Value);
            }
        }
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
        if (Task && Task->OwnerId != ActiveOwnerId)
        {
            Task = nullptr;
        }
        if (!Task || (Command != TEXT("get") && Command != TEXT("cancel")))
        {
            return ErrorPayload(Task
                ? FString::Printf(TEXT("Unknown task command '%s'"), *Command)
                : FString::Printf(TEXT("Unknown task '%s'"), *TaskId));
        }
        if (Command == TEXT("cancel") && Task->State == TEXT("running"))
        {
            FinishTask(*Task, TEXT("cancelled"), TEXT("Cancelled between commands."));
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
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ModelContextProtocol"));
    if (!Plugin)
    {
        UE_LOG(LogModelContextProtocol, Error, TEXT("Unable to locate the ModelContextProtocol plugin directory."));
        return false;
    }
    const FString Path = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/ModelContextProtocol/metadata.json"));
    FString Text;
    if (!FFileHelper::LoadFileToString(Text, *Path))
    {
        UE_LOG(LogModelContextProtocol, Error, TEXT("Unable to read MCP metadata: %s"), *Path);
        return false;
    }
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
    if (!FJsonSerializer::Deserialize(Reader, Metadata) || !Metadata
        || !Metadata->HasTypedField<EJson::Object>(TEXT("tool"))
        || !Metadata->HasTypedField<EJson::Array>(TEXT("capabilities")))
    {
        UE_LOG(LogModelContextProtocol, Error, TEXT("Invalid MCP metadata: %s"), *Path);
        Metadata.Reset();
        return false;
    }
    const TSharedPtr<FJsonObject> Tool = Metadata->GetObjectField(TEXT("tool"));
    if (!Tool->HasTypedField<EJson::Object>(TEXT("inputSchema")))
    {
        UE_LOG(LogModelContextProtocol, Error, TEXT("MCP metadata tool has no inputSchema: %s"), *Path);
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
            if (Value.Equals(Authority, ESearchCase::IgnoreCase))
            {
                return true;
            }
            const FString PortPrefix = Authority + TEXT(":");
            if (Value.StartsWith(PortPrefix, ESearchCase::IgnoreCase))
            {
                const FString PortText = Value.RightChop(PortPrefix.Len());
                if (!PortText.IsEmpty() && PortText.IsNumeric())
                {
                    const int32 ParsedPort = FCString::Atoi(*PortText);
                    return ParsedPort > 0 && ParsedPort <= 65535;
                }
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

FString FUnrealMCPServer::RequestOwnerId(const FHttpServerRequest& Request) const
{
    const auto IsSafeId = [](const FString& Value)
    {
        if (Value.IsEmpty() || Value.Len() > 128)
        {
            return false;
        }
        for (const TCHAR Character : Value)
        {
            if (!FChar::IsAlnum(Character) && Character != TEXT('-')
                && Character != TEXT('_') && Character != TEXT('.'))
            {
                return false;
            }
        }
        return true;
    };

    const FString SessionId = HeaderValue(Request, TEXT("Mcp-Session-Id"));
    if (IsSafeId(SessionId))
    {
        return SessionId;
    }
    const FString ClientId = HeaderValue(Request, TEXT("X-MCP-Client-Id"));
    if (IsSafeId(ClientId))
    {
        return FString::Printf(TEXT("client-%s"), *ClientId);
    }
    return FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
}

TUniquePtr<FHttpServerResponse> FUnrealMCPServer::JsonResponse(
    const TSharedRef<FJsonObject>& Object,
    EHttpServerResponseCodes Code) const
{
    FString Text = JsonToString(Object);
    EHttpServerResponseCodes ResponseCode = Code;
    const FTCHARToUTF8 Encoded(*Text);
    if (Encoded.Length() > MaxResponseBytes)
    {
        Text = JsonToString(JsonRpcError(
            Object->TryGetField(TEXT("id")),
            -32603,
            TEXT("Response exceeds the 4 MiB limit.")));
        ResponseCode = EHttpServerResponseCodes::ServerError;
    }
    TUniquePtr<FHttpServerResponse> Response =
        FHttpServerResponse::Create(Text, TEXT("application/json; charset=utf-8"));
    Response->Code = ResponseCode;
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
