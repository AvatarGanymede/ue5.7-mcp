import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

function source(relativePath: string): string {
  return readFileSync(fileURLToPath(new URL(relativePath, import.meta.url)), "utf8");
}

function section(text: string, start: string, end: string): string {
  const startIndex = text.indexOf(start);
  const endIndex = text.indexOf(end, startIndex + start.length);
  expect(startIndex, `missing section start: ${start}`).toBeGreaterThanOrEqual(0);
  expect(endIndex, `missing section end: ${end}`).toBeGreaterThan(startIndex);
  return text.slice(startIndex, endIndex);
}

const server = source("../ModelContextProtocol/Source/ModelContextProtocol/Private/UnrealMCPServer.cpp");
const serverHeader = source("../ModelContextProtocol/Source/ModelContextProtocol/Private/UnrealMCPServer.h");
const moduleHeader = source("../ModelContextProtocol/Source/ModelContextProtocol/Public/ModelContextProtocolModule.h");

describe("Unreal MCP server hardening contracts", () => {
  it("checks Origin before authentication on HTTP entry points", () => {
    for (const [start, end] of [
      ["bool FUnrealMCPServer::HandleMcpPost", "bool FUnrealMCPServer::HandleMcpGet"],
      ["bool FUnrealMCPServer::HandleMcpGet", "bool FUnrealMCPServer::HandleMcpOptions"],
    ] as const) {
      const handler = section(server, start, end);
      expect(handler.indexOf("IsOriginAllowed(Request)")).toBeLessThan(
        handler.indexOf("IsAuthorized(Request)"),
      );
      expect(handler).toContain("unauthorized");
    }
    expect(
      section(
        server,
        "bool FUnrealMCPServer::HandleMcpOptions",
        "void FUnrealMCPServer::ProcessRequestOnGameThread",
      ),
    ).toContain("IsOriginAllowed(Request)");
  });

  it("binds routes through shared ownership instead of raw server pointers", () => {
    expect(serverHeader).toContain(
      "TSharedFromThis<FUnrealMCPServer, ESPMode::ThreadSafe>",
    );
    expect(moduleHeader).toContain("TSharedPtr<FUnrealMCPServer, ESPMode::ThreadSafe>");
    expect(server).not.toContain("FHttpRequestHandler::CreateRaw(this");
    expect(server.match(/FHttpRequestHandler::CreateSP\(AsShared\(\)/g)).toHaveLength(3);
  });

  it("invalidates delayed work before unbinding routes during shutdown", () => {
    const stop = section(
      server,
      "void FUnrealMCPServer::Stop()",
      "bool FUnrealMCPServer::HandleMcpPost",
    );
    expect(stop.indexOf("Lifetime->AtomicSet(false)")).toBeLessThan(
      stop.indexOf("Router->UnbindRoute"),
    );
    expect(server.match(/\[this, (?:Request|Task)Lifetime/g)?.length ?? 0).toBeGreaterThanOrEqual(3);
  });

  it("observes cancellation and timeout only between asynchronous commands", () => {
    const runner = section(
      server,
      "void FUnrealMCPServer::RunNextTaskCommand",
      "void FUnrealMCPServer::FinishTask",
    );
    expect(runner).toContain('FinishTask(*Task, TEXT("timed_out")');
    expect(runner.indexOf("ExecuteCommand(")).toBeLessThan(runner.indexOf("AddTicker("));
    expect(runner).toContain("RunNextTaskCommand(TaskId)");
    expect(server).toContain('Data->SetBoolField(TEXT("timed_out")');
    expect(server).toContain('FinishTask(*Task, TEXT("cancelled")');
  });

  it("enforces queue, response, query, execution, and task-store limits", () => {
    expect(server).toContain("MaxQueuedRequests = 64");
    expect(server).toContain("MaxResponseBytes = 4 * 1024 * 1024");
    expect(server).toContain("MaxExecutionSeconds = 300.0");
    expect(server).toContain("MaxTasksPerOwner = 128");
    expect(server).toContain("MaxTasks = 1024");
    expect(server).toContain('TEXT("request_queue_full")');
    expect(server).toContain("Encoded.Length() > MaxResponseBytes");
  });

  it("does not claim or attempt atomic rollback for transaction batches", () => {
    expect(server).toContain('Data->SetBoolField(TEXT("transaction_atomic"), false)');
    expect(server).toContain('Data->SetBoolField(TEXT("transaction_rolled_back"), false)');
    expect(server).not.toContain("Transaction->Cancel()");
  });

  it("cleans up partial startup after listener or route registration failure", () => {
    const start = section(server, "bool FUnrealMCPServer::Start()", "void FUnrealMCPServer::Stop()");
    expect(start.match(/\bStop\(\);/g)?.length ?? 0).toBeGreaterThanOrEqual(2);
  });
});
