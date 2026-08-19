import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

type Metadata = {
  tool: { name: string; inputSchema: object };
  official_all_toolsets_plugins: string[];
  capabilities: Array<{ id: string; official_plugins: string[] }>;
};

const metadataPath = fileURLToPath(
  new URL("../UnrealMCP/Resources/UnrealMCP/metadata.json", import.meta.url),
);
const metadata = JSON.parse(readFileSync(metadataPath, "utf8")) as Metadata;

describe("in-editor MCP metadata", () => {
  it("describes exactly one unreal tool", () => {
    expect(metadata.tool.name).toBe("unreal");
    expect(metadata.tool.inputSchema).toBeTypeOf("object");
  });

  it("routes every UE 5.8 AllToolsets plugin", () => {
    expect(metadata.official_all_toolsets_plugins).toHaveLength(21);
    const routed = new Set(metadata.capabilities.flatMap((entry) => entry.official_plugins));
    for (const plugin of metadata.official_all_toolsets_plugins) {
      expect(routed.has(plugin), `missing capability route for ${plugin}`).toBe(true);
    }
  });

  it("uses unique capability ids", () => {
    const ids = metadata.capabilities.map((entry) => entry.id);
    expect(new Set(ids).size).toBe(ids.length);
  });
});
