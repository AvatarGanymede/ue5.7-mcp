#include "WorkerHttpClient.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cwchar>
#include <memory>
#include <vector>

namespace
{
    struct FWinHttpCloser
    {
        void operator()(void* Handle) const
        {
            if (Handle)
            {
                WinHttpCloseHandle(static_cast<HINTERNET>(Handle));
            }
        }
    };

    using FWinHttpHandle = std::unique_ptr<void, FWinHttpCloser>;

    std::wstring GetEnvironmentString(const wchar_t* Name)
    {
        const DWORD Required = GetEnvironmentVariableW(Name, nullptr, 0);
        if (Required == 0)
        {
            return {};
        }
        std::wstring Value(Required, L'\0');
        const DWORD Written = GetEnvironmentVariableW(Name, Value.data(), Required);
        if (Written == 0)
        {
            return {};
        }
        Value.resize(Written);
        return Value;
    }

    std::string Win32ErrorMessage(DWORD Error)
    {
        wchar_t* Buffer = nullptr;
        const DWORD Length = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            Error,
            0,
            reinterpret_cast<wchar_t*>(&Buffer),
            0,
            nullptr);
        if (Length == 0 || !Buffer)
        {
            return "WinHTTP error " + std::to_string(Error);
        }
        const int Utf8Length = WideCharToMultiByte(CP_UTF8, 0, Buffer, static_cast<int>(Length), nullptr, 0, nullptr, nullptr);
        std::string Result(static_cast<size_t>(Utf8Length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, Buffer, static_cast<int>(Length), Result.data(), Utf8Length, nullptr, nullptr);
        LocalFree(Buffer);
        while (!Result.empty() && (Result.back() == '\r' || Result.back() == '\n' || Result.back() == ' '))
        {
            Result.pop_back();
        }
        return Result;
    }

    unsigned short GetWorkerPort()
    {
        const std::wstring Value = GetEnvironmentString(L"UE_MCP_WORKER_PORT");
        if (Value.empty())
        {
            return 18777;
        }
        wchar_t* End = nullptr;
        const unsigned long Parsed = std::wcstoul(Value.c_str(), &End, 10);
        if (!End || *End != L'\0' || Parsed == 0 || Parsed > 65535)
        {
            return 18777;
        }
        return static_cast<unsigned short>(Parsed);
    }
}

int GetDefaultWorkerTimeoutMs()
{
    const std::wstring Value = GetEnvironmentString(L"UE_MCP_TIMEOUT_MS");
    if (Value.empty())
    {
        return 30000;
    }
    wchar_t* End = nullptr;
    const long Parsed = std::wcstol(Value.c_str(), &End, 10);
    return End && *End == L'\0' && Parsed >= 100 && Parsed <= 3600000
        ? static_cast<int>(Parsed)
        : 30000;
}

FWorkerHttpResponse RequestWorker(
    const wchar_t* Method,
    const wchar_t* Path,
    const std::string& Body,
    int TimeoutMs)
{
    FWorkerHttpResponse Result;
    TimeoutMs = std::clamp(TimeoutMs, 100, 3600000);

    FWinHttpHandle Session(WinHttpOpen(
        L"UnrealMCPGateway/0.2.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!Session)
    {
        Result.Error = Win32ErrorMessage(GetLastError());
        return Result;
    }
    WinHttpSetTimeouts(Session.get(), TimeoutMs, TimeoutMs, TimeoutMs, TimeoutMs);

    FWinHttpHandle Connection(WinHttpConnect(Session.get(), L"127.0.0.1", GetWorkerPort(), 0));
    if (!Connection)
    {
        Result.Error = Win32ErrorMessage(GetLastError());
        return Result;
    }

    FWinHttpHandle Request(WinHttpOpenRequest(
        Connection.get(),
        Method,
        Path,
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        0));
    if (!Request)
    {
        Result.Error = Win32ErrorMessage(GetLastError());
        return Result;
    }

    std::wstring Headers = L"Accept: application/json\r\n";
    if (!Body.empty())
    {
        Headers += L"Content-Type: application/json; charset=utf-8\r\n";
    }
    const std::wstring Token = GetEnvironmentString(L"UE_MCP_WORKER_TOKEN");
    if (!Token.empty())
    {
        Headers += L"Authorization: Bearer " + Token + L"\r\n";
    }

    const BOOL Sent = WinHttpSendRequest(
        Request.get(),
        Headers.c_str(),
        static_cast<DWORD>(Headers.size()),
        Body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(Body.data()),
        static_cast<DWORD>(Body.size()),
        static_cast<DWORD>(Body.size()),
        0);
    if (!Sent || !WinHttpReceiveResponse(Request.get(), nullptr))
    {
        Result.Error = Win32ErrorMessage(GetLastError());
        return Result;
    }

    DWORD StatusSize = sizeof(Result.StatusCode);
    if (!WinHttpQueryHeaders(
            Request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &Result.StatusCode,
            &StatusSize,
            WINHTTP_NO_HEADER_INDEX))
    {
        Result.Error = Win32ErrorMessage(GetLastError());
        return Result;
    }

    for (;;)
    {
        DWORD Available = 0;
        if (!WinHttpQueryDataAvailable(Request.get(), &Available))
        {
            Result.Error = Win32ErrorMessage(GetLastError());
            return Result;
        }
        if (Available == 0)
        {
            break;
        }
        const size_t Offset = Result.Body.size();
        Result.Body.resize(Offset + Available);
        DWORD Read = 0;
        if (!WinHttpReadData(Request.get(), Result.Body.data() + Offset, Available, &Read))
        {
            Result.Error = Win32ErrorMessage(GetLastError());
            return Result;
        }
        Result.Body.resize(Offset + Read);
    }

    Result.bTransportOk = true;
    return Result;
}
