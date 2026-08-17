#pragma once

#include <string>

struct FWorkerHttpResponse
{
    bool bTransportOk = false;
    unsigned long StatusCode = 0;
    std::string Body;
    std::string Error;
};

FWorkerHttpResponse RequestWorker(
    const wchar_t* Method,
    const wchar_t* Path,
    const std::string& Body,
    int TimeoutMs);

int GetDefaultWorkerTimeoutMs();

