# UnrealMCP — Native MCP for Unreal Engine 5.7

[English](README.md) | [简体中文](README.zh-CN.md)

## Introduction

UnrealMCP is a self-contained Unreal Engine 5.7 Editor Code Plugin for Win64. It embeds a Streamable HTTP MCP server directly in Unreal Editor, allowing Codex and other local MCP clients to inspect and control the open editor through exactly one MCP tool: `unreal`.

The plugin listens on `http://127.0.0.1:18777/mcp` by default. It runs UObject, Unreal Python, and console operations on the Game Thread and does not require a gateway executable, Node.js, npm, a Python package, or a separately installed runtime service.

Unreal Editor must remain open with the target project loaded while an MCP client is connected.

## Installation

### For Human

1. Download the named `UnrealMCP-...-UE5.7-Win64-GitHub.zip` asset from [GitHub Releases](https://github.com/AvatarGanymede/ue5.7-mcp/releases). Do not use GitHub's automatically generated **Source code** archives as the installer.
2. Close Unreal Editor. When upgrading, replace the complete existing `Plugins/ModelContextProtocol` directory instead of merging files into an older installation. For upgrades from 0.3.x or earlier, remove the legacy `Plugins/UnrealMCP` directory; never keep both plugin identifiers installed.
3. Extract the ZIP into the UE project root, next to the `.uproject` file. The final layout must be:

   ```text
   <Project>/
   ├─ <Project>.uproject
   └─ Plugins/
      └─ ModelContextProtocol/
         ├─ ModelContextProtocol.uplugin
         └─ Binaries/Win64/UnrealEditor-ModelContextProtocol.dll
   ```

4. Open the project, enable **MCP for Unreal Editor** and **Python Editor Script Plugin** under **Edit → Plugins**, then restart Unreal Editor. If an installer or source-control change enables the plugin directly in the `.uproject` file, include the Win64 target explicitly to avoid UE 5.7's empty-target warning:

   ```json
   {
     "Name": "ModelContextProtocol",
     "Enabled": true,
     "SupportedTargetPlatforms": ["Win64"]
   }
   ```

5. The server uses port `18777` by default. If this project may be open alongside another Unreal project using UnrealMCP, choose a unique port under **Edit → Project Settings → Plugins → MCP for Unreal Editor**, then restart Unreal Editor. Add the matching MCP endpoint to Codex:

   ```bash
   codex mcp add unreal --url http://127.0.0.1:18777/mcp
   ```

   Or add it to `~/.codex/config.toml` or a trusted project's `.codex/config.toml`:

   ```toml
   [mcp_servers.unreal]
   url = "http://127.0.0.1:18777/mcp"
   tool_timeout_sec = 3600
   ```

   `UE_MCP_PORT` may still be set before launching Unreal Editor as a temporary per-process override. The configured project port is used when the environment variable is absent.

6. Restart Codex, confirm that `unreal` is connected, and call the `health` action.

### For Agent

When installing UnrealMCP for a user, follow this procedure:

1. Locate the target project root by finding its `.uproject` file. Use a project-local installation unless the user explicitly requests an engine-wide installation.
2. Obtain the latest named release asset matching `UnrealMCP-*-UE5.7-Win64-GitHub.zip`. Never substitute GitHub's automatic source archive.
3. Ask the user to close Unreal Editor before replacing binaries. Do not overwrite an active plugin installation.
4. Inspect the archive before extraction and verify that it contains `Plugins/ModelContextProtocol/ModelContextProtocol.uplugin` and `Plugins/ModelContextProtocol/Binaries/Win64/UnrealEditor-ModelContextProtocol.dll`.
5. Replace the complete `<Project>/Plugins/ModelContextProtocol` directory with the archive's plugin directory. When migrating from 0.3.x or earlier, remove `<Project>/Plugins/UnrealMCP` so the legacy and new plugin identifiers cannot load together. Preserve unrelated plugins and project files. If editing the `.uproject` directly, preserve its other entries and use `{ "Name": "ModelContextProtocol", "Enabled": true, "SupportedTargetPlatforms": ["Win64"] }` for this plugin reference.
6. Following the current agent's own MCP configuration conventions, register a Streamable HTTP server named `unreal` with the port selected under **Project Settings → Plugins → MCP for Unreal Editor** (`18777` by default) in that agent's **project-scoped MCP list**. For example: `http://127.0.0.1:18777/mcp`. Do not register it at user or global scope, and do not overwrite unrelated project MCP entries. Set the tool timeout to 3600 seconds if the agent supports that setting.
7. Tell the user to enable **MCP for Unreal Editor** and **Python Editor Script Plugin**, then restart Unreal Editor.
8. After Unreal Editor restarts, call:

   ```json
   { "action": "health" }
   ```

   A ready server reports `ok: true`, `is_game_thread: true`, and `python_loaded: true`.
9. Verify `tools/list` exposes `unreal` and that the host presents it as a directly callable tool. If the host does not refresh project MCP tools, reconnect/restart the host. For diagnostics from Git Bash, use `scripts/unreal-mcp.sh --list`; the helper avoids hand-written JSON-RPC envelopes but is not required at runtime.

### Build from source with Git Bash

The named GitHub Release ZIP is the preferred installer. To build a checkout locally on Windows, run from Git Bash:

```bash
scripts/build-plugin.sh --engine-root 'C:/Program Files/Epic Games/UE_5.7'
```

The script prints its fresh package directory under `artifacts/`. It searches the selected UE installation for the newest bundled `Engine/Binaries/ThirdParty/DotNet/*/win-x64/dotnet.exe`, places that runtime first on `PATH`, sets `DOTNET_ROOT`, and disables multilevel lookup before invoking `RunUAT.sh BuildPlugin`. This prevents an unrelated system `dotnet` from being selected without `Microsoft.WindowsDesktop.App 8.x`. Use `--output PATH` for a specific fresh directory or `UE_DOTNET_ROOT` to override bundled runtime discovery for diagnostics.

## Configuration

### Configure the server port

The MCP server listens on `127.0.0.1:18777` by default. Give each Unreal project a different port when multiple projects may be open at the same time:

1. Open the project in Unreal Editor.
2. Go to **Edit → Project Settings → Plugins → MCP for Unreal Editor**.
3. Under **Server**, set **Port** to an unused value from `1` through `65535`, such as `18778`.
4. Restart Unreal Editor. The HTTP listener uses the new port only after the editor restarts.
5. Update this project's MCP client URL to use the same port, then reconnect or restart the client.

For example, if the project setting is `18778`, use this project-scoped Codex configuration:

```toml
[mcp_servers.unreal]
url = "http://127.0.0.1:18778/mcp"
tool_timeout_sec = 3600
```

For two simultaneously open projects, Project A can keep `18777` while Project B uses `18778`. Their MCP client URLs must match their respective project settings.

For automation or a one-time launch, set `UE_MCP_PORT` before starting Unreal Editor. A valid `UE_MCP_PORT` overrides the project setting for that editor process only; removing the environment variable restores the configured project port on the next launch.

After restarting, call the `health` action and check its `endpoint` field to confirm the active port:

```json
{ "action": "health" }
```

## MCP Toolset

The server exposes one MCP tool named `unreal`. Its `action` field selects one of four operations:

| Action | Purpose |
|---|---|
| `health` | Check the server, engine version, Game Thread dispatch, Python availability, transport, and endpoint. |
| `discover` | Search capability domains, runnable UE 5.7 API starting points, and discovered plugin mount status. |
| `execute` | Run up to 100 ordered Python, console, native Blueprint graph, or asynchronous non-blocking wait commands. |
| `task` | Get, list, or cancel asynchronous work submitted through `execute`. |

Check the connection:

```json
{ "action": "health" }
```

When the MCP host has not surfaced `unreal` directly, the repository helper can call the same endpoint from Git Bash:

```bash
scripts/unreal-mcp.sh '{"action":"health"}'
scripts/unreal-mcp.sh --list
```

Discover a workflow before choosing UE APIs:

```json
{
  "action": "discover",
  "query": "create and compile a blueprint",
  "limit": 5
}
```

Execute an ordered batch on the Game Thread:

```json
{
  "action": "execute",
  "run": "sync",
  "transaction": true,
  "continue_on_error": false,
  "commands": [
    {
      "kind": "python",
      "mode": "eval",
      "label": "engine-version",
      "code": "unreal.SystemLibrary.get_engine_version()"
    },
    {
      "kind": "console",
      "label": "show-fps",
      "command": "stat fps"
    }
  ]
}
```

Blueprint asset/component/default editing and visible K2 Event Graph authoring are separate capabilities. Use Python and `BlueprintEditorLibrary` for assets, variables, components, and class defaults. Use `kind: "blueprint_graph"` for visible nodes and connections; this native UE 5.7 backend follows the design of UE 5.8's `BlueprintGraphEditor`/`BlueprintGraphPin` APIs and uses Blueprint node spawners plus the K2 schema rather than unsupported Python wrappers.

```json
{
  "action": "execute",
  "transaction": true,
  "commands": [
    {
      "kind": "blueprint_graph",
      "operation": "add_function_call",
      "blueprint_path": "/Game/Blueprints/BP_Example.BP_Example",
      "function_path": "/Script/Engine.KismetSystemLibrary.PrintString",
      "x": 320,
      "y": 0
    },
    {
      "kind": "blueprint_graph",
      "operation": "compile",
      "blueprint_path": "/Game/Blueprints/BP_Example.BP_Example",
      "save": true
    }
  ]
}
```

Start with `operation: "inspect"`; every node and pin includes a GUID plus pin direction/name/index data. `connect`, `disconnect`, and `set_pin_default` accept the returned pin GUID, while pin references can also use the UE 5.8-style node/direction/name/index tuple to survive ordinary pin-index drift. Supported operations cover inspection, standard/custom events, function calls, member variable get/set nodes, connections, defaults, movement, comments, removal, compilation, and saving. Timeline/track authoring is not currently supported.

For long-running work, set `"run": "async"`; the response returns a `task_id`. Inspect it with:

```json
{ "action": "task", "command": "get", "task_id": "<uuid>" }
```

Runtime assertions that must observe later ticks can use non-blocking wait commands. Waits require `run=async`; synchronous waits are rejected instead of sleeping on the Game Thread:

```json
{
  "action": "execute",
  "run": "async",
  "transaction": false,
  "commands": [
    { "kind": "python", "mode": "exec", "code": "world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world(); unreal.GameplayStatics.get_player_pawn(world, 0).jump()" },
    { "kind": "wait", "frames": 1, "label": "next-tick" },
    { "kind": "wait", "seconds": 0.08, "label": "jump-window" },
    { "kind": "python", "mode": "eval", "code": "(lambda pawn: (pawn.get_velocity().z, pawn.get_character_movement().is_falling()))(unreal.GameplayStatics.get_player_pawn(unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world(), 0))" }
  ]
}
```

Async batches execute one command per Game Thread tick. Their 300-second wall-clock limit and cooperative cancellation are checked only between commands; an in-flight Python or console command is never interrupted. `transaction: true` records each Python and console command separately for Undo; `wait` does not add an Undo entry. Python `eval` is not assumed to be read-only because an expression can call mutating UObject methods. Transactions are not atomic, and Python can change objects before raising. Failures, timeouts, and cancellation do not automatically roll back; inspect `partial_changes_possible`, `commands_completed`, `commands_succeeded`, and `failed_command_index`, then clean up idempotently when needed. Python failures expose the full traceback in both the command `result` and `error` fields.

The server validates tool arguments against the published JSON Schema and applies fixed safety limits: 4 MiB request and response bodies, 64 queued HTTP requests, 4096-character discovery queries, 100 commands per batch, 128 tasks per MCP session/client, and 1024 tasks globally. Async task list/get/cancel operations are isolated by `Mcp-Session-Id` (or the client identity fallback when no MCP session header is available).

The discovery catalog covers editor and asset operations, Blueprint asset/default editing and native K2 graph authoring, AI and navigation, animation, automation, configuration, conversations, Data Registry, Dataflow, Game Features, Gameplay Tags and GAS, Niagara, PCG, physics, plugins, semantic search, Slate, StateTree, UMG, World Conditions, and project-specific reflected APIs such as UnLua. High-friction PIE, collision, current-animation, player-pawn, viewport-widget, and Blueprint graph workflows include runnable recipes. A plugin-name query also reports discovered plugins whether enabled or disabled, including `enabled`, `mounted`, `can_contain_content`, `content_dir`, and `mounted_asset_path`; this helps diagnose assets that Asset Registry cannot see until plugin content is mounted. It does not recursively index files inside disabled plugin directories. Availability depends on the corresponding UE 5.7 or project plugin being enabled.
