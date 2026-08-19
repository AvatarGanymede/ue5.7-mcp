import { Client, StreamableHTTPClientTransport } from "@modelcontextprotocol/client";
import { afterEach, describe, expect, it } from "vitest";

const serverUrl = process.env.UE_MCP_TEST_URL;
const serverToken = process.env.UE_MCP_TEST_TOKEN;
const httpIt = serverUrl ? it : it.skip;

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
});
