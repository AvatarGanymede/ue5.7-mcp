import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

type Metadata = {
  tool: {
    name: string;
    inputSchema: {
      oneOf: Array<{
        properties?: Record<string, Record<string, unknown>>;
        required?: string[];
        oneOf?: Array<{ required?: string[] }>;
      }>;
    };
  };
  official_all_toolsets_plugins: string[];
  capabilities: Array<{ id: string; official_plugins: string[] }>;
};

const metadataPath = fileURLToPath(
  new URL("../ModelContextProtocol/Resources/ModelContextProtocol/metadata.json", import.meta.url),
);
const metadata = JSON.parse(readFileSync(metadataPath, "utf8")) as Metadata;
const packageMetadata = JSON.parse(source("../package.json")) as { version: string };
const pluginDescriptor = JSON.parse(
  source("../ModelContextProtocol/ModelContextProtocol.uplugin"),
) as { Version: number; VersionName: string };

function source(relativePath: string): string {
  return readFileSync(fileURLToPath(new URL(relativePath, import.meta.url)), "utf8");
}

describe("in-editor MCP metadata", () => {
  it("describes exactly one unreal tool", () => {
    expect(metadata.tool.name).toBe("unreal");
    expect(metadata.tool.inputSchema).toBeTypeOf("object");
  });

  it("keeps release version references aligned", () => {
    expect(packageMetadata.version).toBe(pluginDescriptor.VersionName);
    expect(pluginDescriptor.VersionName).toMatch(/^\d+\.\d+\.\d+$/);
    expect(pluginDescriptor.Version).toBeGreaterThan(0);
    expect(source("../ModelContextProtocol/THIRD_PARTY_NOTICES.md")).toContain(
      `UnrealMCP ${pluginDescriptor.VersionName}`,
    );
    expect(source("../scripts/build-github-package.ps1")).toContain(
      `UnrealMCP-${pluginDescriptor.VersionName}-UE5.7-Win64-GitHub.zip`,
    );
    expect(source("../scripts/build-fab-package.ps1")).toContain(
      `UnrealMCP-${pluginDescriptor.VersionName}-UE5.7-Win64.zip`,
    );
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

  it("caps discovery queries at 4096 characters", () => {
    const discover = metadata.tool.inputSchema.oneOf.find(
      (branch) => branch.properties?.action?.const === "discover",
    );
    expect(discover?.properties?.query?.maxLength).toBe(4096);
  });

  it("documents non-atomic transactions and command-boundary cancellation", () => {
    const execute = metadata.tool.inputSchema.oneOf.find(
      (branch) => branch.properties?.action?.const === "execute",
    );
    const taskGetOrCancel = metadata.tool.inputSchema.oneOf.find(
      (branch) => Array.isArray(branch.properties?.command?.enum),
    );
    const taskList = metadata.tool.inputSchema.oneOf.find(
      (branch) => branch.properties?.command?.const === "list",
    );

    expect(execute?.properties?.transaction?.description).toContain("not atomic");
    expect(execute?.properties?.run?.description).toContain("between commands");
    expect(execute?.properties?.transaction?.description).toContain("partial changes");
    expect(taskGetOrCancel?.properties?.command?.description).toContain("only between commands");
    expect(taskGetOrCancel?.required).toEqual(["action", "command", "task_id"]);
    expect(taskList?.properties?.task_id?.description).toContain("backward compatibility");
  });

  it("offers non-blocking waits and runnable UE 5.7 recipes", () => {
    const execute = metadata.tool.inputSchema.oneOf.find(
      (branch) => branch.properties?.action?.const === "execute",
    );
    const commandVariants = execute?.properties?.commands?.items as {
      oneOf?: Array<{ properties?: Record<string, Record<string, unknown>> }>;
    } | undefined;
    const waitVariants = commandVariants?.oneOf?.filter(
      (branch) => branch.properties?.kind?.const === "wait",
    );
    expect(waitVariants).toHaveLength(2);
    expect(waitVariants?.some((branch) => branch.properties?.frames)).toBe(true);
    expect(waitVariants?.some((branch) => branch.properties?.seconds)).toBe(true);

    const actorWorld = metadata.capabilities.find((entry) => entry.id === "actor-world") as
      | { recipes?: Array<{ name: string; code: string }> }
      | undefined;
    expect(actorWorld?.recipes?.map((recipe) => recipe.name)).toContain("trigger_pawn_overlap");
    expect(JSON.stringify(actorWorld?.recipes)).toContain("CollisionResponseType.ECR_OVERLAP");
  });
});
