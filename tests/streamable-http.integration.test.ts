import { Client, StreamableHTTPClientTransport } from "@modelcontextprotocol/client";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { afterEach, describe, expect, it } from "vitest";

const serverUrl = process.env.UE_MCP_TEST_URL;
const serverToken = process.env.UE_MCP_TEST_TOKEN;
const httpIt = serverUrl ? it : it.skip;
const authenticatedHttpIt = serverUrl && serverToken ? it : it.skip;
const packageVersion = (JSON.parse(readFileSync(
  fileURLToPath(new URL("../package.json", import.meta.url)),
  "utf8",
)) as { version: string }).version;

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
        transaction_recorded?: boolean;
        transaction_atomic?: boolean;
        transaction_rolled_back?: boolean;
        timed_out?: boolean;
        partial_changes_possible?: boolean;
        commands_completed?: number;
        commands_succeeded?: number;
        failed_command_index?: number;
        matched?: number;
        results?: Array<{
          id?: string;
          kind?: string;
          error?: string;
          value?: {
            ok?: boolean;
            errors?: number;
            saved?: boolean;
            node?: GraphNode;
            nodes?: GraphNode[];
          };
        }>;
        plugins?: Array<{
          name: string;
          enabled: boolean;
          mounted: boolean;
          can_contain_content: boolean;
          content_dir: string;
          mounted_asset_path: string;
        }>;
      };
    };
  };
};

type GraphPin = {
  id: string;
  name: string;
  direction: "input" | "output";
  category: string;
  index?: number;
  links: Array<{ node_id: string; pin_id: string }>;
};

type GraphNode = {
  id: string;
  class: string;
  title: string;
  pins: GraphPin[];
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

    expect(client.getServerVersion()?.version).toBe(packageVersion);
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

    const legacyList = await rawToolCall(session, {
      action: "task",
      command: "list",
      task_id: "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee",
    });
    expect(legacyList.result?.isError).toBe(false);
    expect(legacyList.result?.structuredContent?.ok).toBe(true);
  });

  httpIt("ranks actionable discovery results and reports plugin mount diagnostics", async () => {
    const session = "acacacac-acac-4cac-8cac-acacacacacac";
    const discovered = await rawToolCall(session, {
      action: "discover",
      query: "list actors in the current editor level and inspect transforms",
      limit: 3,
    });
    expect(discovered.result?.structuredContent?.data?.results?.[0]?.id).toBe("actor-world");
    expect(discovered.result?.structuredContent?.data?.matched).toBeGreaterThan(0);

    const plugins = await rawToolCall(session, {
      action: "discover",
      domain: "plugins",
      query: "ModelContextProtocol",
      limit: 3,
    });
    expect(plugins.result?.structuredContent?.data?.plugins).toContainEqual(
      expect.objectContaining({
        name: "ModelContextProtocol",
        enabled: true,
        mounted: true,
        can_contain_content: false,
      }),
    );

    const evalQuery = await rawToolCall(session, {
      action: "execute",
      transaction: true,
      commands: [{ kind: "python", mode: "eval", code: "1 + 1" }],
    });
    expect(evalQuery.result?.structuredContent?.data?.transaction_recorded).toBe(true);
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

  httpIt("waits across ticks without blocking asynchronous runtime checks", async () => {
    const session = "abababab-abab-4bab-8bab-abababababab";
    const sync = await rawToolCall(session, {
      action: "execute",
      run: "sync",
      transaction: false,
      commands: [{ kind: "wait", seconds: 0.05 }],
    });
    expect(sync.result?.isError).toBe(true);
    expect(sync.result?.structuredContent?.error).toContain("run=async");

    const startedAt = Date.now();
    const created = await rawToolCall(session, {
      action: "execute",
      run: "async",
      transaction: true,
      commands: [
        { kind: "python", mode: "eval", code: "unreal.SystemLibrary.get_engine_version()" },
        { kind: "wait", frames: 2, label: "two-ticks" },
        { kind: "wait", seconds: 0.05, label: "runtime-window" },
        { kind: "python", mode: "eval", code: "unreal.SystemLibrary.get_engine_version()" },
      ],
    });
    const taskId = created.result?.structuredContent?.data?.task?.id;
    expect(taskId).toBeTruthy();

    let snapshot: JsonRpcResponse | undefined;
    for (let attempt = 0; attempt < 50; ++attempt) {
      snapshot = await rawToolCall(session, {
        action: "task",
        command: "get",
        task_id: taskId,
      });
      if (snapshot.result?.structuredContent?.data?.task?.state !== "running") break;
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
    const task = snapshot?.result?.structuredContent?.data?.task;
    expect(task?.state).toBe("succeeded");
    expect(Date.now() - startedAt).toBeGreaterThanOrEqual(40);
    expect(JSON.stringify(task?.result?.results)).toContain('"kind":"wait"');
    expect(task?.result?.transaction_atomic).toBe(false);
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
      data: {
        transaction_atomic: false,
        transaction_rolled_back: false,
        partial_changes_possible: true,
        commands_completed: 2,
        commands_succeeded: 1,
        failed_command_index: 1,
      },
    });
    expect(response.result?.structuredContent?.data?.results).toHaveLength(2);
    expect(JSON.stringify(response)).toContain('"error":"Traceback');

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

  httpIt("authors, compiles, saves, and re-inspects visible Blueprint graph logic", async () => {
    const session = "bdbdbdbd-bdbd-4dbd-8dbd-bdbdbdbdbdbd";
    const suffix = Date.now().toString(36);
    const assetName = `BP_MCPGraph_${suffix}`;
    const packagePath = `/Game/UnrealMCPTests/${assetName}`;
    const objectPath = `${packagePath}.${assetName}`;

    const executeGraph = async (command: Record<string, unknown>) => {
      const response = await rawToolCall(session, {
        action: "execute",
        transaction: true,
        commands: [{ kind: "blueprint_graph", blueprint_path: objectPath, ...command }],
      });
      expect(response.result?.isError).toBe(false);
      const result = response.result?.structuredContent?.data?.results?.[0];
      expect(result?.kind).toBe("blueprint_graph");
      expect(result?.value?.ok).toBe(true);
      return result!.value!;
    };

    try {
      const create = await rawToolCall(session, {
        action: "execute",
        transaction: false,
        commands: [{
          kind: "python",
          mode: "exec",
          code: [
            "folder = '/Game/UnrealMCPTests'",
            `name = '${assetName}'`,
            "unreal.EditorAssetLibrary.make_directory(folder)",
            "factory = unreal.BlueprintFactory()",
            "factory.set_editor_property('parent_class', unreal.Actor)",
            "asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, folder, unreal.Blueprint, factory)",
            "assert asset is not None",
            "unreal.BlueprintEditorLibrary.compile_blueprint(asset)",
            "assert unreal.EditorAssetLibrary.save_loaded_asset(asset)",
          ].join("; "),
        }],
      });
      expect(create.result?.isError).toBe(false);

      const event = await executeGraph({
        operation: "add_event",
        event_name: "ReceiveBeginPlay",
        event_class: "/Script/Engine.Actor",
        x: 0,
        y: 0,
      });
      const call = await executeGraph({
        operation: "add_function_call",
        function_path: "/Script/Engine.KismetSystemLibrary.PrintString",
        x: 320,
        y: 0,
      });
      const eventNode = event.node!;
      const callNode = call.node!;
      const eventExec = eventNode.pins.find(
        (pin) => pin.direction === "output" && pin.category === "exec",
      );
      const callExec = callNode.pins.find(
        (pin) => pin.direction === "input" && pin.category === "exec",
      );
      expect(eventExec).toBeTruthy();
      expect(callExec).toBeTruthy();

      await executeGraph({
        operation: "connect",
        from_pin_id: eventExec!.id,
        to_pin_id: callExec!.id,
      });
      const compiled = await executeGraph({ operation: "compile", save: true });
      expect(compiled.errors).toBe(0);
      expect(compiled.saved).toBe(true);

      const inspected = await executeGraph({ operation: "inspect" });
      const reloadedEvent = inspected.nodes?.find((node) => node.id === eventNode.id);
      const reloadedCall = inspected.nodes?.find((node) => node.id === callNode.id);
      expect(reloadedEvent).toBeTruthy();
      expect(reloadedCall).toBeTruthy();
      expect(reloadedEvent?.pins.flatMap((pin) => pin.links))
        .toContainEqual(expect.objectContaining({ node_id: callNode.id }));
    }
    finally {
      await rawToolCall(session, {
        action: "execute",
        transaction: false,
        commands: [{
          kind: "python",
          mode: "eval",
          code: `unreal.EditorAssetLibrary.delete_asset('${packagePath}')`,
        }],
      });
    }
  });

});
