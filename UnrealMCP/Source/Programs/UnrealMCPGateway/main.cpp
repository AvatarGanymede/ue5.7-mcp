#include "WorkerHttpClient.h"

#include <windows.h>
#include <objbase.h>
#include <fcntl.h>
#include <io.h>

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using rapidjson::Document;
using rapidjson::SizeType;
using rapidjson::Value;

namespace
{
    constexpr size_t MaxStdioMessageBytes = 4u * 1024u * 1024u;
    constexpr const char* LatestProtocolVersion = "2026-07-28";
    constexpr const char* ServerInstructions =
        "One tool is exposed. Use action=discover for unfamiliar Unreal APIs, then action=execute with a compact Python/console batch. "
        "Use eval for queries, exec for mutations, and async plus task.get for long work. UObject/editor calls run on UE's Game Thread.";

    Value JsonString(const std::string& Text, Document::AllocatorType& Allocator)
    {
        return Value(Text.c_str(), static_cast<SizeType>(Text.size()), Allocator);
    }

    std::string SerializeJson(const Value& Json, bool bPretty = false)
    {
        rapidjson::StringBuffer Buffer;
        if (bPretty)
        {
            rapidjson::PrettyWriter<rapidjson::StringBuffer> Writer(Buffer);
            Json.Accept(Writer);
        }
        else
        {
            rapidjson::Writer<rapidjson::StringBuffer> Writer(Buffer);
            Json.Accept(Writer);
        }
        return {Buffer.GetString(), Buffer.GetSize()};
    }

    Document ParseJson(const std::string& Text)
    {
        Document Result;
        Result.Parse(Text.data(), Text.size());
        return Result;
    }

    std::string LowerAscii(std::string Text)
    {
        std::transform(Text.begin(), Text.end(), Text.begin(), [](unsigned char Character)
        {
            return static_cast<char>(std::tolower(Character));
        });
        return Text;
    }

    std::vector<std::string> QueryTerms(const std::string& Query)
    {
        std::vector<std::string> Terms;
        std::string Current;
        for (const unsigned char Character : LowerAscii(Query))
        {
            if (std::isalnum(Character) || Character >= 0x80 || Character == '-' || Character == '_')
            {
                Current.push_back(static_cast<char>(Character));
            }
            else if (!Current.empty())
            {
                Terms.push_back(std::move(Current));
                Current.clear();
            }
        }
        if (!Current.empty())
        {
            Terms.push_back(std::move(Current));
        }
        return Terms;
    }

    std::string IsoTimestamp()
    {
        SYSTEMTIME Time;
        GetSystemTime(&Time);
        char Buffer[32];
        std::snprintf(
            Buffer,
            sizeof(Buffer),
            "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
            Time.wYear,
            Time.wMonth,
            Time.wDay,
            Time.wHour,
            Time.wMinute,
            Time.wSecond,
            Time.wMilliseconds);
        return Buffer;
    }

    std::string NewUuid()
    {
        GUID Id{};
        if (FAILED(CoCreateGuid(&Id)))
        {
            static std::atomic<unsigned long long> Counter{0};
            return "00000000-0000-4000-8000-" + std::to_string(++Counter);
        }
        char Buffer[37];
        std::snprintf(
            Buffer,
            sizeof(Buffer),
            "%08lx-%04x-%04x-%04x-%012llx",
            Id.Data1,
            Id.Data2,
            Id.Data3,
            static_cast<unsigned int>((Id.Data4[0] << 8) | Id.Data4[1]),
            (static_cast<unsigned long long>(Id.Data4[2]) << 40) |
            (static_cast<unsigned long long>(Id.Data4[3]) << 32) |
            (static_cast<unsigned long long>(Id.Data4[4]) << 24) |
            (static_cast<unsigned long long>(Id.Data4[5]) << 16) |
            (static_cast<unsigned long long>(Id.Data4[6]) << 8) |
            static_cast<unsigned long long>(Id.Data4[7]));
        return LowerAscii(Buffer);
    }

    std::filesystem::path ExecutablePath()
    {
        std::wstring Buffer(32768, L'\0');
        const DWORD Length = GetModuleFileNameW(nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
        if (Length == 0 || Length >= Buffer.size())
        {
            return {};
        }
        Buffer.resize(Length);
        return std::filesystem::path(Buffer);
    }

    std::filesystem::path MetadataPath()
    {
        const std::filesystem::path BinaryDirectory = ExecutablePath().parent_path();
        return BinaryDirectory.parent_path().parent_path() / L"Resources" / L"UnrealMCP" / L"metadata.json";
    }

    bool IsJsonObjectWithOk(const Document& Json)
    {
        return Json.IsObject() && Json.HasMember("ok") && Json["ok"].IsBool() && Json["ok"].GetBool();
    }

    Document ErrorPayload(const std::string& Message)
    {
        Document Payload(rapidjson::kObjectType);
        auto& Allocator = Payload.GetAllocator();
        Payload.AddMember("ok", false, Allocator);
        Payload.AddMember("error", JsonString(Message, Allocator), Allocator);
        return Payload;
    }

    Document WorkerPayload(const FWorkerHttpResponse& Response, const char* Kind)
    {
        if (!Response.bTransportOk)
        {
            return ErrorPayload(std::string("Cannot reach UE worker: ") + Response.Error);
        }

        Document Worker = ParseJson(Response.Body);
        if (Worker.HasParseError() || !Worker.IsObject())
        {
            return ErrorPayload("UE worker returned invalid JSON (HTTP " + std::to_string(Response.StatusCode) + ")");
        }

        const bool bOk = Response.StatusCode >= 200 && Response.StatusCode < 300 && IsJsonObjectWithOk(Worker);
        Document Payload(rapidjson::kObjectType);
        auto& Allocator = Payload.GetAllocator();
        Payload.AddMember("ok", bOk, Allocator);
        Value Data(rapidjson::kObjectType);
        if (std::string(Kind) == "health")
        {
            Data.AddMember("gateway", "ok", Allocator);
            Value WorkerCopy;
            WorkerCopy.CopyFrom(Worker, Allocator);
            Data.AddMember("worker", WorkerCopy, Allocator);
        }
        else
        {
            Data.CopyFrom(Worker, Allocator);
        }
        Payload.AddMember("data", Data, Allocator);
        if (!bOk)
        {
            Payload.AddMember("error", JsonString("UE worker rejected the request (HTTP " + std::to_string(Response.StatusCode) + ")", Allocator), Allocator);
        }
        return Payload;
    }

    std::string WorkerExecuteBody(const Value& Arguments)
    {
        Document Body(rapidjson::kObjectType);
        auto& Allocator = Body.GetAllocator();
        Value Commands(rapidjson::kArrayType);
        if (Arguments.IsObject() && Arguments.HasMember("commands") && Arguments["commands"].IsArray())
        {
            Commands.CopyFrom(Arguments["commands"], Allocator);
        }
        Body.AddMember("commands", Commands, Allocator);
        const bool bTransaction = !Arguments.HasMember("transaction") || !Arguments["transaction"].IsBool() || Arguments["transaction"].GetBool();
        const bool bContinueOnError = Arguments.HasMember("continue_on_error") && Arguments["continue_on_error"].IsBool() && Arguments["continue_on_error"].GetBool();
        Body.AddMember("transaction", bTransaction, Allocator);
        Body.AddMember("continue_on_error", bContinueOnError, Allocator);
        return SerializeJson(Body);
    }

    int RequestTimeout(const Value& Arguments)
    {
        if (Arguments.IsObject() && Arguments.HasMember("timeout_ms") && Arguments["timeout_ms"].IsInt())
        {
            return std::clamp(Arguments["timeout_ms"].GetInt(), 100, 3600000);
        }
        return GetDefaultWorkerTimeoutMs();
    }

    struct FTask
    {
        std::string Id;
        std::string State = "running";
        std::string CreatedAt;
        std::string UpdatedAt;
        std::string ResultJson;
        std::string Error;
    };

    class FTaskStore : public std::enable_shared_from_this<FTaskStore>
    {
    public:
        FTask Start(std::string Body, int TimeoutMs)
        {
            auto Task = std::make_shared<FTask>();
            Task->Id = NewUuid();
            Task->CreatedAt = IsoTimestamp();
            Task->UpdatedAt = Task->CreatedAt;
            {
                std::lock_guard Lock(Mutex);
                PruneLocked();
                Tasks.emplace(Task->Id, Task);
            }
            const FTask InitialSnapshot = *Task;

            const std::shared_ptr<FTaskStore> Self = shared_from_this();
            std::thread([Self, Task, Body = std::move(Body), TimeoutMs]()
            {
                const FWorkerHttpResponse Response = RequestWorker(L"POST", L"/ue-mcp/v1/execute", Body, TimeoutMs);
                const Document Payload = WorkerPayload(Response, "execute");
                std::lock_guard Lock(Self->Mutex);
                if (Task->State == "cancelled")
                {
                    return;
                }
                Task->UpdatedAt = IsoTimestamp();
                Task->ResultJson = Payload.HasMember("data") ? SerializeJson(Payload["data"]) : std::string();
                if (IsJsonObjectWithOk(Payload))
                {
                    Task->State = "succeeded";
                }
                else
                {
                    Task->State = "failed";
                    if (Payload.HasMember("error") && Payload["error"].IsString())
                    {
                        Task->Error = Payload["error"].GetString();
                    }
                }
            }).detach();
            return InitialSnapshot;
        }

        std::optional<FTask> Get(const std::string& Id)
        {
            std::lock_guard Lock(Mutex);
            const auto Found = Tasks.find(Id);
            return Found == Tasks.end() ? std::nullopt : std::optional<FTask>(*Found->second);
        }

        std::vector<FTask> List()
        {
            std::lock_guard Lock(Mutex);
            std::vector<FTask> Result;
            Result.reserve(Tasks.size());
            for (const auto& Entry : Tasks)
            {
                Result.push_back(*Entry.second);
            }
            std::sort(Result.begin(), Result.end(), [](const auto& Left, const auto& Right)
            {
                return Left.CreatedAt > Right.CreatedAt;
            });
            return Result;
        }

        std::optional<FTask> Cancel(const std::string& Id)
        {
            std::lock_guard Lock(Mutex);
            const auto Found = Tasks.find(Id);
            if (Found == Tasks.end())
            {
                return std::nullopt;
            }
            if (Found->second->State == "running")
            {
                // WinHTTP may already have dispatched the batch to UE. Cancellation
                // suppresses its result but cannot roll back editor side effects.
                Found->second->State = "cancelled";
                Found->second->UpdatedAt = IsoTimestamp();
            }
            return *Found->second;
        }

    private:
        void PruneLocked()
        {
            if (Tasks.size() < 128)
            {
                return;
            }
            for (auto Iterator = Tasks.begin(); Iterator != Tasks.end() && Tasks.size() >= 128;)
            {
                if (Iterator->second->State != "running")
                {
                    Iterator = Tasks.erase(Iterator);
                }
                else
                {
                    ++Iterator;
                }
            }
        }

        std::mutex Mutex;
        std::unordered_map<std::string, std::shared_ptr<FTask>> Tasks;
    };

    Value TaskSnapshot(const FTask& Task, Document::AllocatorType& Allocator)
    {
        Value Result(rapidjson::kObjectType);
        Result.AddMember("id", JsonString(Task.Id, Allocator), Allocator);
        Result.AddMember("state", JsonString(Task.State, Allocator), Allocator);
        Result.AddMember("created_at", JsonString(Task.CreatedAt, Allocator), Allocator);
        Result.AddMember("updated_at", JsonString(Task.UpdatedAt, Allocator), Allocator);
        if (!Task.ResultJson.empty())
        {
            Document Parsed = ParseJson(Task.ResultJson);
            if (!Parsed.HasParseError())
            {
                Value Copy;
                Copy.CopyFrom(Parsed, Allocator);
                Result.AddMember("result", Copy, Allocator);
            }
        }
        if (!Task.Error.empty())
        {
            Result.AddMember("error", JsonString(Task.Error, Allocator), Allocator);
        }
        return Result;
    }

    class FMcpGateway
    {
    public:
        bool Load()
        {
            const std::filesystem::path Path = MetadataPath();
            std::ifstream Stream(Path, std::ios::binary);
            if (!Stream)
            {
                std::cerr << "UnrealMCPGateway: metadata not found: " << Path.string() << '\n';
                return false;
            }
            std::ostringstream Buffer;
            Buffer << Stream.rdbuf();
            Metadata = ParseJson(Buffer.str());
            if (Metadata.HasParseError() || !Metadata.IsObject() || !Metadata.HasMember("tool") || !Metadata.HasMember("capabilities"))
            {
                std::cerr << "UnrealMCPGateway: metadata.json is invalid\n";
                return false;
            }
            return true;
        }

        std::string ProcessLine(const std::string& Line)
        {
            Document Request = ParseJson(Line);
            if (Request.HasParseError())
            {
                return SerializeJson(JsonRpcError(nullptr, -32700, "Parse error"));
            }
            if (!Request.IsObject() || !Request.HasMember("method") || !Request["method"].IsString())
            {
                const Value* Id = Request.IsObject() && Request.HasMember("id") ? &Request["id"] : nullptr;
                return SerializeJson(JsonRpcError(Id, -32600, "Invalid Request"));
            }

            const char* Method = Request["method"].GetString();
            const bool bHasId = Request.HasMember("id");
            if (std::string(Method) == "notifications/initialized" ||
                std::string(Method) == "notifications/cancelled")
            {
                return {};
            }
            if (!bHasId)
            {
                return {};
            }

            const Value& Id = Request["id"];
            if (std::string(Method) == "server/discover")
            {
                Document Result = DiscoverServerResult();
                AddModernResultFields(Result);
                return SerializeJson(JsonRpcResult(Id, Result));
            }
            if (std::string(Method) == "initialize")
            {
                return SerializeJson(JsonRpcResult(Id, InitializeResult(Request)));
            }
            if (std::string(Method) == "ping")
            {
                Document Empty(rapidjson::kObjectType);
                if (IsModernRequest(Request)) AddModernResultFields(Empty);
                return SerializeJson(JsonRpcResult(Id, Empty));
            }
            if (std::string(Method) == "tools/list")
            {
                Document Result = ToolsListResult();
                if (IsModernRequest(Request)) AddModernResultFields(Result);
                return SerializeJson(JsonRpcResult(Id, Result));
            }
            if (std::string(Method) == "tools/call")
            {
                Document Result = ToolsCallResult(Request);
                if (IsModernRequest(Request)) AddModernResultFields(Result);
                return SerializeJson(JsonRpcResult(Id, Result));
            }
            return SerializeJson(JsonRpcError(&Id, -32601, "Method not found"));
        }

    private:
        bool IsModernRequest(const Document& Request) const
        {
            if (!Request.HasMember("params") || !Request["params"].IsObject()) return false;
            const Value& Params = Request["params"];
            if (!Params.HasMember("_meta") || !Params["_meta"].IsObject()) return false;
            const Value& Meta = Params["_meta"];
            if (!Meta.HasMember("io.modelcontextprotocol/protocolVersion") ||
                !Meta["io.modelcontextprotocol/protocolVersion"].IsString()) return false;
            return std::string(Meta["io.modelcontextprotocol/protocolVersion"].GetString()) >= LatestProtocolVersion;
        }

        void AddModernResultFields(Document& Result) const
        {
            auto& Allocator = Result.GetAllocator();
            if (!Result.HasMember("resultType")) Result.AddMember("resultType", "complete", Allocator);
            if (!Result.HasMember("ttlMs")) Result.AddMember("ttlMs", 0, Allocator);
            if (!Result.HasMember("cacheScope")) Result.AddMember("cacheScope", "private", Allocator);
            if (!Result.HasMember("_meta"))
            {
                Value Meta(rapidjson::kObjectType);
                Value ServerInfo(rapidjson::kObjectType);
                ServerInfo.AddMember("name", "ue57-mcp-native", Allocator);
                ServerInfo.AddMember("title", "Unreal Editor MCP", Allocator);
                ServerInfo.AddMember("version", "0.2.0", Allocator);
                Meta.AddMember("io.modelcontextprotocol/serverInfo", ServerInfo, Allocator);
                Result.AddMember("_meta", Meta, Allocator);
            }
        }

        Document DiscoverServerResult()
        {
            Document Result(rapidjson::kObjectType);
            auto& Allocator = Result.GetAllocator();
            Value Versions(rapidjson::kArrayType);
            Versions.PushBack(Value(LatestProtocolVersion, Allocator), Allocator);
            Result.AddMember("supportedVersions", Versions, Allocator);
            Value Capabilities(rapidjson::kObjectType);
            Value Tools(rapidjson::kObjectType);
            Tools.AddMember("listChanged", false, Allocator);
            Capabilities.AddMember("tools", Tools, Allocator);
            Result.AddMember("capabilities", Capabilities, Allocator);
            Result.AddMember("instructions", JsonString(ServerInstructions, Allocator), Allocator);
            return Result;
        }

        Document InitializeResult(const Document& Request)
        {
            // initialize is the legacy handshake. Modern clients negotiate with
            // server/discover and do not initialize the session process.
            std::string Protocol = "2025-11-25";
            if (Request.HasMember("params") && Request["params"].IsObject())
            {
                const Value& Params = Request["params"];
                if (Params.HasMember("protocolVersion") && Params["protocolVersion"].IsString())
                {
                    const std::string Requested = Params["protocolVersion"].GetString();
                    static const std::vector<std::string> Supported = {
                        "2026-07-28", "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
                    if (std::find(Supported.begin(), Supported.end(), Requested) != Supported.end())
                    {
                        Protocol = Requested;
                    }
                }
            }

            Document Result(rapidjson::kObjectType);
            auto& Allocator = Result.GetAllocator();
            Result.AddMember("protocolVersion", JsonString(Protocol, Allocator), Allocator);
            Value Capabilities(rapidjson::kObjectType);
            Value Tools(rapidjson::kObjectType);
            Tools.AddMember("listChanged", false, Allocator);
            Capabilities.AddMember("tools", Tools, Allocator);
            Result.AddMember("capabilities", Capabilities, Allocator);
            Value ServerInfo(rapidjson::kObjectType);
            ServerInfo.AddMember("name", "ue57-mcp-native", Allocator);
            ServerInfo.AddMember("title", "Unreal Editor MCP", Allocator);
            ServerInfo.AddMember("version", "0.2.0", Allocator);
            Result.AddMember("serverInfo", ServerInfo, Allocator);
            Result.AddMember("instructions", JsonString(ServerInstructions, Allocator), Allocator);
            return Result;
        }

        Document ToolsListResult()
        {
            Document Result(rapidjson::kObjectType);
            auto& Allocator = Result.GetAllocator();
            Value Tools(rapidjson::kArrayType);
            Value Tool;
            Tool.CopyFrom(Metadata["tool"], Allocator);
            Tools.PushBack(Tool, Allocator);
            Result.AddMember("tools", Tools, Allocator);
            return Result;
        }

        Document ToolsCallResult(const Document& Request)
        {
            Document Payload;
            if (!Request.HasMember("params") || !Request["params"].IsObject())
            {
                Payload = ErrorPayload("tools/call requires params");
            }
            else
            {
                const Value& Params = Request["params"];
                if (!Params.HasMember("name") || !Params["name"].IsString() || std::string(Params["name"].GetString()) != "unreal")
                {
                    Payload = ErrorPayload("Unknown tool; expected 'unreal'");
                }
                else if (!Params.HasMember("arguments") || !Params["arguments"].IsObject())
                {
                    Payload = ErrorPayload("unreal requires an arguments object");
                }
                else
                {
                    Payload = HandleAction(Params["arguments"]);
                }
            }

            Document Result(rapidjson::kObjectType);
            auto& Allocator = Result.GetAllocator();
            const bool bOk = IsJsonObjectWithOk(Payload);
            Value Content(rapidjson::kArrayType);
            Value TextContent(rapidjson::kObjectType);
            TextContent.AddMember("type", "text", Allocator);
            TextContent.AddMember("text", JsonString(SerializeJson(Payload, true), Allocator), Allocator);
            Content.PushBack(TextContent, Allocator);
            Result.AddMember("content", Content, Allocator);
            Value Structured;
            Structured.CopyFrom(Payload, Allocator);
            Result.AddMember("structuredContent", Structured, Allocator);
            Result.AddMember("isError", !bOk, Allocator);
            return Result;
        }

        Document HandleAction(const Value& Arguments)
        {
            if (!Arguments.HasMember("action") || !Arguments["action"].IsString())
            {
                return ErrorPayload("action is required");
            }
            const std::string Action = Arguments["action"].GetString();
            if (Action == "discover")
            {
                return Discover(Arguments);
            }
            if (Action == "health")
            {
                return WorkerPayload(RequestWorker(L"GET", L"/ue-mcp/v1/health", {}, GetDefaultWorkerTimeoutMs()), "health");
            }
            if (Action == "execute")
            {
                if (!Arguments.HasMember("commands") || !Arguments["commands"].IsArray() || Arguments["commands"].Empty())
                {
                    return ErrorPayload("execute requires a non-empty commands array");
                }
                if (Arguments["commands"].Size() > 100)
                {
                    return ErrorPayload("execute accepts at most 100 commands");
                }
                const std::string Body = WorkerExecuteBody(Arguments);
                const int Timeout = RequestTimeout(Arguments);
                const bool bAsync = Arguments.HasMember("run") && Arguments["run"].IsString() && std::string(Arguments["run"].GetString()) == "async";
                if (bAsync)
                {
                    const FTask Task = Tasks->Start(Body, Timeout);
                    Document Payload(rapidjson::kObjectType);
                    auto& Allocator = Payload.GetAllocator();
                    Payload.AddMember("ok", true, Allocator);
                    Value Data(rapidjson::kObjectType);
                    Data.AddMember("task", TaskSnapshot(Task, Allocator), Allocator);
                    Payload.AddMember("data", Data, Allocator);
                    return Payload;
                }
                return WorkerPayload(RequestWorker(L"POST", L"/ue-mcp/v1/execute", Body, Timeout), "execute");
            }
            if (Action == "task")
            {
                return HandleTask(Arguments);
            }
            return ErrorPayload("Unknown action '" + Action + "'");
        }

        Document Discover(const Value& Arguments)
        {
            const std::string Domain = Arguments.HasMember("domain") && Arguments["domain"].IsString()
                ? LowerAscii(Arguments["domain"].GetString())
                : std::string();
            const std::string Query = Arguments.HasMember("query") && Arguments["query"].IsString()
                ? Arguments["query"].GetString()
                : std::string();
            const std::vector<std::string> Terms = QueryTerms(Query);
            int Limit = Arguments.HasMember("limit") && Arguments["limit"].IsInt() ? Arguments["limit"].GetInt() : 12;
            Limit = std::clamp(Limit, 1, 50);

            struct FMatch { int Score; const Value* Capability; };
            std::vector<FMatch> Matches;
            for (const Value& Capability : Metadata["capabilities"].GetArray())
            {
                if (!Capability.IsObject() || !Capability.HasMember("id") || !Capability["id"].IsString())
                {
                    continue;
                }
                const std::string Id = LowerAscii(Capability["id"].GetString());
                if (!Domain.empty() && Id != Domain)
                {
                    continue;
                }
                std::string Haystack = SerializeJson(Capability);
                Haystack = LowerAscii(std::move(Haystack));
                int Score = Domain == Id ? 1000 : 0;
                for (const std::string& Term : Terms)
                {
                    if (Id == Term) Score += 50;
                    if (Haystack.find(Term) != std::string::npos) Score += 10;
                }
                if (Terms.empty() || Score > 0)
                {
                    Matches.push_back({Score, &Capability});
                }
            }
            std::stable_sort(Matches.begin(), Matches.end(), [](const FMatch& Left, const FMatch& Right)
            {
                return Left.Score > Right.Score;
            });

            Document Payload(rapidjson::kObjectType);
            auto& Allocator = Payload.GetAllocator();
            Payload.AddMember("ok", true, Allocator);
            Value Data(rapidjson::kObjectType);
            Data.AddMember("exposed_mcp_tools", 1, Allocator);
            Data.AddMember("capability_domains", static_cast<unsigned>(Metadata["capabilities"].Size()), Allocator);
            Value Official;
            Official.CopyFrom(Metadata["official_all_toolsets_plugins"], Allocator);
            Data.AddMember("official_all_toolsets_plugins", Official, Allocator);
            Value Results(rapidjson::kArrayType);
            for (size_t Index = 0; Index < Matches.size() && Index < static_cast<size_t>(Limit); ++Index)
            {
                Value Copy;
                Copy.CopyFrom(*Matches[Index].Capability, Allocator);
                Results.PushBack(Copy, Allocator);
            }
            Data.AddMember("results", Results, Allocator);
            Payload.AddMember("data", Data, Allocator);
            return Payload;
        }

        Document HandleTask(const Value& Arguments)
        {
            if (!Arguments.HasMember("command") || !Arguments["command"].IsString())
            {
                return ErrorPayload("task.command is required");
            }
            const std::string Command = Arguments["command"].GetString();
            Document Payload(rapidjson::kObjectType);
            auto& Allocator = Payload.GetAllocator();
            Payload.AddMember("ok", true, Allocator);
            Value Data(rapidjson::kObjectType);
            if (Command == "list")
            {
                Value Snapshots(rapidjson::kArrayType);
                for (const FTask& Task : Tasks->List())
                {
                    Snapshots.PushBack(TaskSnapshot(Task, Allocator), Allocator);
                }
                Data.AddMember("tasks", Snapshots, Allocator);
            }
            else
            {
                if (!Arguments.HasMember("task_id") || !Arguments["task_id"].IsString())
                {
                    return ErrorPayload("task_id is required for task." + Command);
                }
                const std::string Id = Arguments["task_id"].GetString();
                const std::optional<FTask> Task = Command == "cancel" ? Tasks->Cancel(Id) : Tasks->Get(Id);
                if (!Task || (Command != "get" && Command != "cancel"))
                {
                    return ErrorPayload(Task ? "Unknown task command '" + Command + "'" : "Unknown task '" + Id + "'");
                }
                Data.AddMember("task", TaskSnapshot(*Task, Allocator), Allocator);
            }
            Payload.AddMember("data", Data, Allocator);
            return Payload;
        }

        Document JsonRpcResult(const Value& Id, const Value& ResultValue)
        {
            Document Response(rapidjson::kObjectType);
            auto& Allocator = Response.GetAllocator();
            Response.AddMember("jsonrpc", "2.0", Allocator);
            Value IdCopy;
            IdCopy.CopyFrom(Id, Allocator);
            Response.AddMember("id", IdCopy, Allocator);
            Value ResultCopy;
            ResultCopy.CopyFrom(ResultValue, Allocator);
            Response.AddMember("result", ResultCopy, Allocator);
            return Response;
        }

        Document JsonRpcError(const Value* Id, int Code, const std::string& Message)
        {
            Document Response(rapidjson::kObjectType);
            auto& Allocator = Response.GetAllocator();
            Response.AddMember("jsonrpc", "2.0", Allocator);
            if (Id)
            {
                Value IdCopy;
                IdCopy.CopyFrom(*Id, Allocator);
                Response.AddMember("id", IdCopy, Allocator);
            }
            else
            {
                Response.AddMember("id", Value(rapidjson::kNullType), Allocator);
            }
            Value Error(rapidjson::kObjectType);
            Error.AddMember("code", Code, Allocator);
            Error.AddMember("message", JsonString(Message, Allocator), Allocator);
            Response.AddMember("error", Error, Allocator);
            return Response;
        }

        Document Metadata;
        std::shared_ptr<FTaskStore> Tasks = std::make_shared<FTaskStore>();
    };
}

int main()
{
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    std::ios::sync_with_stdio(false);

    FMcpGateway Gateway;
    if (!Gateway.Load())
    {
        return 2;
    }

    std::string Line;
    while (std::getline(std::cin, Line))
    {
        if (!Line.empty() && Line.back() == '\r')
        {
            Line.pop_back();
        }
        if (Line.empty())
        {
            continue;
        }
        if (Line.size() > MaxStdioMessageBytes)
        {
            std::cerr << "UnrealMCPGateway: rejected stdio message larger than 4 MiB\n";
            continue;
        }
        const std::string Response = Gateway.ProcessLine(Line);
        if (!Response.empty())
        {
            std::cout << Response << '\n' << std::flush;
        }
    }
    return 0;
}
