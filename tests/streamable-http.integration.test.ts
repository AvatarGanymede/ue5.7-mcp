import { Client, StreamableHTTPClientTransport } from "@modelcontextprotocol/client";
import { afterEach, describe, expect, it } from "vitest";

const serverUrl = process.env.UE_MCP_TEST_URL;
const serverToken = process.env.UE_MCP_TEST_TOKEN;
const httpIt = serverUrl ? it : it.skip;
const authenticatedHttpIt = serverUrl && serverToken ? it : it.skip;

type JsonRpcResponse = {
  result?: {
    isError?: boolean;
    structuredContent?: {
      ok: boolean;
      error?: string;
      data?: {
        task?: {
          id: string;
          state: string;
          error?: string;
          result?: { results?: unknown[]; transaction_atomic?: boolean };
        };
        tasks?: Array<{ id: string; state: string }>;
        results?: unknown[];
        transaction_recorded?: boolean;
        transaction_atomic?: boolean;
        transaction_rolled_back?: boolean;
        timed_out?: boolean;
      };
    };
  };
};

let rpcId = 100;

function requestHeaders(extra: Record<string, string> = {}): Record<string, string> {
  return {
    "Content-Type": "application/json",
    Accept: "application/json",
    ...(serverToken ? { Authorization: `Bearer ${serverToken}` } : {}),
    ...extra,
  };
}

async function rawToolCall(sessionId: string, args: object): Promise<JsonRpcResponse> {
  const response = await fetch(serverUrl!, {
    method: "POST",
    headers: requestHeaders({ "Mcp-Session-Id": sessionId }),
    body: JSON.stringify({
      jsonrpc: "2.0",
      id: rpcId++,
      method: "tools/call",
      params: { name: "unreal", arguments: args },
    }),
  });
  expect(response.status).toBe(200);
  return (await response.json()) as JsonRpcResponse;
}

describe("in-editor Streamable HTTP MCP server", () => {
  const clients: Client[] = [];

  afterEach(async () => {
    await Promise.all(clients.splice(0).map((client) => client.close()));
  });

  httpIt("negotiates MCP and exposes exactly one unreal tool", async () => {
    const transport = new StreamableHTTPClientTransport(
      new URL(serverUrl!),
      serverToken
        ? { requestInit: { headers: { Authorization: `Bearer ${serverToken}` } } }
        : undefined,
    );
    const client = new Client({ name: "ue57-http-test", version: "1.0.0" });
    clients.push(client);
    await client.connect(transport);

    expect((await client.listTools()).tools.map((tool) => tool.name)).toEqual(["unreal"]);
    const health = await client.callTool({ name: "unreal", arguments: { action: "health" } });
    expect(health.isError).toBe(false);
    expect(health.structuredContent).toMatchObject({
      ok: true,
      data: { transport: "streamable-http", is_game_thread: true },
    });
  });

  httpIt("enforces the published unreal input JSON Schema", async () => {
    const session = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee";
    const invalidArguments = [
      { action: "health", unexpected: true },
      { action: "discover", limit: 1.5 },
      { action: "execute", commands: [{ kind: "python", code: "" }] },
      { action: "task", command: "get" },
      { action: "task", command: "get", task_id: "not-a-uuid" },
    ];
    for (const args of invalidArguments) {
      const response = await rawToolCall(session, args);
      expect(response.result?.isError).toBe(true);
      expect(response.result?.structuredContent?.ok).toBe(false);
      expect(response.result?.structuredContent?.error).toContain("Invalid arguments:");
    }
  });

  authenticatedHttpIt("rejects missing and invalid bearer credentials", async () => {
    const body = JSON.stringify({ jsonrpc: "2.0", id: rpcId++, method: "ping" });
    for (const authorization of [undefined, "Bearer definitely-wrong"]) {
      const response = await fetch(serverUrl!, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          ...(authorization ? { Authorization: authorization } : {}),
        },
        body,
      });
      expect(response.status).toBe(401);
      expect(response.headers.get("www-authenticate")).toBe("Bearer");
    }
  });

  httpIt("rejects non-local and authority-confusion Origins", async () => {
    const body = JSON.stringify({ jsonrpc: "2.0", id: rpcId++, method: "ping" });
    for (const origin of [
      "https://attacker.example",
      "http://localhost.example",
      "http://localhost:18777@attacker.example",
    ]) {
      const response = await fetch(serverUrl!, {
        method: "POST",
        headers: requestHeaders({ Origin: origin }),
        body,
      });
      expect(response.status).toBe(403);
    }
  });

  httpIt("isolates async task listing, lookup, and cancellation by MCP session", async () => {
    const ownerA = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
    const ownerB = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
    const created = await rawToolCall(ownerB, {
      action: "execute",
      run: "async",
      transaction: false,
      commands: Array.from({ length: 100 }, () => ({
        kind: "python",
        mode: "eval",
        code: "1 + 1",
      })),
    });
    const taskId = created.result?.structuredContent?.data?.task?.id;
    expect(taskId).toMatch(/^[0-9a-f-]{36}$/);

    const foreignList = await rawToolCall(ownerA, { action: "task", command: "list" });
    expect(foreignList.result?.structuredContent?.data?.tasks).not.toContainEqual(
      expect.objectContaining({ id: taskId }),
    );
    for (const command of ["get", "cancel"] as const) {
      const foreignAccess = await rawToolCall(ownerA, {
        action: "task",
        command,
        task_id: taskId,
      });
      expect(foreignAccess.result?.isError).toBe(true);
      expect(foreignAccess.result?.structuredContent?.ok).toBe(false);
    }

    const ownerAccess = await rawToolCall(ownerB, {
      action: "task",
      command: "get",
      task_id: taskId,
    });
    expect(ownerAccess.result?.structuredContent?.ok).toBe(true);
    expect(ownerAccess.result?.structuredContent?.data?.task?.id).toBe(taskId);
  });

  httpIt("cancels asynchronous work only at a command boundary", async () => {
    const session = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
    const created = await rawToolCall(session, {
      action: "execute",
      run: "async",
      transaction: false,
      commands: Array.from({ length: 100 }, () => ({
        kind: "python",
        mode: "eval",
        code: "sum(range(1000000))",
      })),
    });
    const taskId = created.result?.structuredContent?.data?.task?.id;
    expect(taskId).toBeTruthy();

    const cancelled = await rawToolCall(session, {
      action: "task",
      command: "cancel",
      task_id: taskId,
    });
    const cancelledTask = cancelled.result?.structuredContent?.data?.task;
    expect(cancelledTask?.state).toBe("cancelled");
    const completedAtCancel = cancelledTask?.result?.results?.length ?? 0;
    expect(completedAtCancel).toBeLessThan(100);

    await new Promise((resolve) => setTimeout(resolve, 100));
    const afterDelay = await rawToolCall(session, {
      action: "task",
      command: "get",
      task_id: taskId,
    });
    expect(afterDelay.result?.structuredContent?.data?.task?.state).toBe("cancelled");
    expect(afterDelay.result?.structuredContent?.data?.task?.result?.results).toHaveLength(
      completedAtCancel,
    );
  });

  httpIt("keeps completed command effects when a later transactional command fails", async () => {
    const session = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
    const response = await rawToolCall(session, {
      action: "execute",
      transaction: true,
      commands: [
        {
          kind: "python",
          mode: "exec",
          code: "import builtins; builtins._ue_mcp_rollback_probe = 'preserved'",
        },
        { kind: "python", mode: "exec", code: "raise RuntimeError('expected test failure')" },
      ],
    });
    expect(response.result?.isError).toBe(true);
    expect(response.result?.structuredContent).toMatchObject({
      ok: false,
      data: { transaction_atomic: false, transaction_rolled_back: false },
    });
    expect(response.result?.structuredContent?.data?.results).toHaveLength(2);

    const probe = await rawToolCall(session, {
      action: "execute",
      transaction: false,
      commands: [{
        kind: "python",
        mode: "eval",
        code: "getattr(__import__('builtins'), '_ue_mcp_rollback_probe', None)",
      }],
    });
    expect(JSON.stringify(probe)).toContain("preserved");
    await rawToolCall(session, {
      action: "execute",
      transaction: false,
      commands: [{
        kind: "python",
        mode: "exec",
        code: "import builtins; del builtins._ue_mcp_rollback_probe",
      }],
    });
  });

});
