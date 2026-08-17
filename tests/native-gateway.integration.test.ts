import { Client } from "@modelcontextprotocol/client";
import { StdioClientTransport } from "@modelcontextprotocol/client/stdio";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { afterEach, describe, expect, it } from "vitest";

const gatewayPath = process.env.UE_MCP_TEST_GATEWAY ?? fileURLToPath(
  new URL("../UnrealMCP/Binaries/Win64/UnrealMCPGateway.exe", import.meta.url),
);
const nativeIt = process.platform === "win32" && existsSync(gatewayPath) ? it : it.skip;

describe("native C++ stdio gateway", () => {
  const clients: Client[] = [];
  afterEach(async () => {
    await Promise.all(clients.splice(0).map((client) => client.close()));
  });

  nativeIt("negotiates the modern protocol and exposes exactly one tool", async () => {
    const transport = new StdioClientTransport({ command: gatewayPath, stderr: "pipe" });
    const client = new Client(
      { name: "native-modern-test", version: "1.0.0" },
      { versionNegotiation: { mode: "auto" } },
    );
    clients.push(client);
    await client.connect(transport);

    expect(client.getProtocolEra()).toBe("modern");
    expect((await client.listTools()).tools.map((tool) => tool.name)).toEqual(["unreal"]);

    const discovery = await client.callTool({
      name: "unreal",
      arguments: { action: "discover", query: "PCG graph nodes", limit: 3 },
    });
    expect(discovery.isError).toBe(false);
    expect(discovery.structuredContent).toMatchObject({
      ok: true,
      data: { exposed_mcp_tools: 1 },
    });
  });

  nativeIt("accepts the legacy initialization flow", async () => {
    const transport = new StdioClientTransport({ command: gatewayPath, stderr: "pipe" });
    const client = new Client({ name: "native-legacy-test", version: "1.0.0" });
    clients.push(client);
    await client.connect(transport);

    expect(client.getProtocolEra()).toBe("legacy");
    expect((await client.listTools()).tools.map((tool) => tool.name)).toEqual(["unreal"]);
  });
});
