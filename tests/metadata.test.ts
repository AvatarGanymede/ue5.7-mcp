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
  capabilities: Array<{
    id: string;
    official_plugins: string[];
    summary?: string;
    backend?: string;
    recipes?: Array<{ name: string; command?: Record<string, unknown>; code?: string }>;
  }>;
};

const metadataPath = fileURLToPath(
  new URL("../ModelContextProtocol/Resources/ModelContextProtocol/metadata.json", import.meta.url),
);
const metadata = JSON.parse(readFileSync(metadataPath, "utf8")) as Metadata;
const packageMetadata = JSON.parse(source("../package.json")) as { version: string };
const pluginDescriptor = JSON.parse(
  source("../ModelContextProtocol/ModelContextProtocol.uplugin"),
) as { Version: number; VersionName: string };
const hostProject = JSON.parse(source("../UE57MCPTest.uproject")) as {
  Plugins: Array<{
    Name: string;
    Enabled: boolean;
    SupportedTargetPlatforms?: string[];
  }>;
};

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

    const releaseWorkflow = source("../.github/workflows/release.yml");
    expect(releaseWorkflow).toContain('UnrealMCP-$version-UE5.7-Win64-GitHub.zip');
    expect(releaseWorkflow).toContain("scripts/build-github-package.sh");
    expect(releaseWorkflow).toContain("scripts/test-release-asset.sh");
    expect(source("../README.md")).toContain("UnrealMCP-...-UE5.7-Win64-GitHub.zip");
    expect(source("../README.zh-CN.md")).toContain(
      "UnrealMCP-...-UE5.7-Win64-GitHub.zip",
    );
  });

  it("exposes a project setting for the MCP server port", () => {
    const settingsHeader = source(
      "../ModelContextProtocol/Source/ModelContextProtocol/Public/ModelContextProtocolSettings.h",
    );
    expect(settingsHeader).toContain("UCLASS(Config=ModelContextProtocol, DefaultConfig");
    expect(settingsHeader).toContain('return TEXT("Plugins")');
    expect(settingsHeader).toContain("ConfigRestartRequired=true");
    expect(settingsHeader).toContain("int32 Port = 18777");

    const serverSource = source(
      "../ModelContextProtocol/Source/ModelContextProtocol/Private/UnrealMCPServer.cpp",
    );
    expect(serverSource).toContain("GetDefault<UModelContextProtocolSettings>()");
    expect(serverSource.indexOf("Settings->Port")).toBeLessThan(
      serverSource.indexOf('GetEnvironmentVariable(TEXT("UE_MCP_PORT"))'),
    );
    expect(source("../ModelContextProtocol/Source/ModelContextProtocol/ModelContextProtocol.Build.cs"))
      .toContain('"DeveloperSettings"');
    expect(source("../ModelContextProtocol/Config/DefaultModelContextProtocol.ini"))
      .toContain("[/Script/ModelContextProtocol.ModelContextProtocolSettings]\nPort=18777");
  });

  it("provides a Git Bash build path pinned to UE-bundled .NET", () => {
    const buildScript = source("../scripts/build-plugin.sh");
    expect(buildScript).toContain("Engine/Binaries/ThirdParty/DotNet");
    expect(buildScript).toContain("export DOTNET_ROOT");
    expect(buildScript).toContain("export DOTNET_MULTILEVEL_LOOKUP=0");
    expect(buildScript).toContain("Microsoft.WindowsDesktop.App 8");
    expect(buildScript).toContain('"$uat" BuildPlugin');
    expect(buildScript).toContain("UnrealEditor.modules");
    expect(source("../.gitattributes")).toContain("*.sh text eol=lf");
    expect(source("../README.md")).toContain("scripts/build-plugin.sh --engine-root");
    expect(source("../README.zh-CN.md")).toContain("scripts/build-plugin.sh --engine-root");
  });

  it("builds and smoke-tests the final release asset without repository binaries", () => {
    const buildScript = source("../scripts/build-plugin.sh");
    expect(buildScript).toContain("--exclude='./Binaries'");
    expect(buildScript).toContain("--exclude='./Intermediate'");

    const packageScript = source("../scripts/build-github-package.sh");
    expect(packageScript).toContain('"$repository_root/scripts/build-plugin.sh"');
    expect(packageScript).toContain("tar --format zip");
    expect(packageScript).not.toContain("$repository_root/ModelContextProtocol/Binaries");

    const legacyPackageScript = source("../scripts/package-prebuilt-github-release.ps1");
    expect(legacyPackageScript).toContain("Repository Binaries are not release inputs");
    expect(legacyPackageScript).not.toContain("Copy-Item -LiteralPath $sourcePlugin");

    const smokeScript = source("../scripts/test-release-asset.sh");
    expect(smokeScript).toContain("unzip -q \"$asset\"");
    expect(smokeScript).toContain("unreal.load_class(None, settings_path)");
    expect(smokeScript).toContain('settings.get_editor_property("port")');
    expect(smokeScript).toContain("unreal.ObjectIterator(unreal.Class)");
    expect(smokeScript).toContain('"default_port": 18777');
  });

  it("declares Win64 on explicit project plugin references", () => {
    const pluginReference = hostProject.Plugins.find(
      (plugin) => plugin.Name === "ModelContextProtocol",
    );
    expect(pluginReference).toMatchObject({
      Enabled: true,
      SupportedTargetPlatforms: ["Win64"],
    });
    expect(source("../scripts/test-http-e2e.ps1")).toContain(
      "SupportedTargetPlatforms = @('Win64')",
    );
    expect(source("../README.md")).toContain('"SupportedTargetPlatforms": ["Win64"]');
    expect(source("../README.zh-CN.md")).toContain(
      '"SupportedTargetPlatforms": ["Win64"]',
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

  it("separates Blueprint defaults from native visible K2 graph authoring", () => {
    const execute = metadata.tool.inputSchema.oneOf.find(
      (branch) => branch.properties?.action?.const === "execute",
    );
    const commandVariants = execute?.properties?.commands?.items as {
      oneOf?: Array<{ properties?: Record<string, Record<string, unknown>> }>;
    } | undefined;
    const graphCommand = commandVariants?.oneOf?.find(
      (branch) => branch.properties?.kind?.const === "blueprint_graph",
    );
    const operations = graphCommand?.properties?.operation?.enum as string[] | undefined;
    expect(operations).toEqual(expect.arrayContaining([
      "inspect",
      "add_event",
      "add_custom_event",
      "add_function_call",
      "add_variable_get",
      "add_variable_set",
      "connect",
      "disconnect",
      "set_pin_default",
      "remove_node",
      "move_node",
      "set_comment",
      "compile",
    ]));

    const blueprint = metadata.capabilities.find((entry) => entry.id === "blueprint");
    expect(blueprint?.backend).toBe("python+native-blueprint-graph");
    expect(blueprint?.summary).toContain("component configuration alone is not Event Graph logic");
    expect(blueprint?.summary).toContain("Timeline track authoring is not supported");
    expect(blueprint?.recipes?.map((recipe) => recipe.name)).toEqual(
      expect.arrayContaining(["inspect_event_graph", "add_begin_play", "compile_and_save"]),
    );

    const graphSource = source(
      "../ModelContextProtocol/Source/ModelContextProtocol/Private/BlueprintGraphOperations.cpp",
    );
    expect(graphSource).toContain("UBlueprintFunctionNodeSpawner::Create");
    expect(graphSource).toContain("UBlueprintVariableNodeSpawner::CreateFromMemberOrParam");
    expect(graphSource).toContain("MarkBlueprintDirtyFromNewNode");
    expect(graphSource).toContain("TryCreateConnection");
    expect(source("../ModelContextProtocol/Source/ModelContextProtocol/ModelContextProtocol.Build.cs"))
      .toContain('"BlueprintGraph"');
    expect(source("../README.md")).toContain("BlueprintGraphEditor");
    expect(source("../README.zh-CN.md")).toContain("BlueprintGraphEditor");
  });
});
