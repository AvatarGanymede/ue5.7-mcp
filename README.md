# UnrealMCP — native MCP for Unreal Engine 5.7

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

4. Open the project, enable **MCP for Unreal Editor** and **Python Editor Script Plugin** under **Edit → Plugins**, then restart Unreal Editor.
5. Add the MCP endpoint to Codex:

   ```powershell
   codex mcp add unreal --url http://127.0.0.1:18777/mcp
   ```

   Or add it to `~/.codex/config.toml` or a trusted project's `.codex/config.toml`:

   ```toml
   [mcp_servers.unreal]
   url = "http://127.0.0.1:18777/mcp"
   tool_timeout_sec = 3600
   ```

6. Restart Codex, confirm that `unreal` is connected, and call the `health` action.

### For Agent

When installing UnrealMCP for a user, follow this procedure:

1. Locate the target project root by finding its `.uproject` file. Use a project-local installation unless the user explicitly requests an engine-wide installation.
2. Obtain the latest named release asset matching `UnrealMCP-*-UE5.7-Win64-GitHub.zip`. Never substitute GitHub's automatic source archive.
3. Ask the user to close Unreal Editor before replacing binaries. Do not overwrite an active plugin installation.
4. Inspect the archive before extraction and verify that it contains `Plugins/ModelContextProtocol/ModelContextProtocol.uplugin` and `Plugins/ModelContextProtocol/Binaries/Win64/UnrealEditor-ModelContextProtocol.dll`.
5. Replace the complete `<Project>/Plugins/ModelContextProtocol` directory with the archive's plugin directory. When migrating from 0.3.x or earlier, remove `<Project>/Plugins/UnrealMCP` so the legacy and new plugin identifiers cannot load together. Preserve unrelated plugins and project files.
6. Following the current agent's own MCP configuration conventions, register a Streamable HTTP server named `unreal` with URL `http://127.0.0.1:18777/mcp` in that agent's **project-scoped MCP list**. Do not register it at user or global scope, and do not overwrite unrelated project MCP entries. Set the tool timeout to 3600 seconds if the agent supports that setting.
7. Tell the user to enable **MCP for Unreal Editor** and **Python Editor Script Plugin**, then restart Unreal Editor.
8. After Unreal Editor restarts, call:

   ```json
   { "action": "health" }
   ```

   A ready server reports `ok: true`, `is_game_thread: true`, and `python_loaded: true`.

## MCP Toolset

The server exposes one MCP tool named `unreal`. Its `action` field selects one of four operations:

| Action | Purpose |
|---|---|
| `health` | Check the server, engine version, Game Thread dispatch, Python availability, transport, and endpoint. |
| `discover` | Search the capability catalog for relevant Unreal APIs, subsystems, console commands, and example workflows. |
| `execute` | Run an ordered batch of up to 100 Unreal Python or console commands, synchronously or asynchronously. |
| `task` | Get, list, or cancel asynchronous work submitted through `execute`. |

Check the connection:

```json
{ "action": "health" }
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

For long-running work, set `"run": "async"`; the response returns a `task_id`. Inspect it with:

```json
{ "action": "task", "command": "get", "task_id": "<uuid>" }
```

Async batches execute one command per Game Thread tick. Their 300-second wall-clock limit and cooperative cancellation are checked only between commands; an in-flight Python or console command is never interrupted. `transaction: true` records each command separately for Undo and is not an atomic batch—failure, timeout, or cancellation does not roll back commands that already finished.

The server validates tool arguments against the published JSON Schema and applies fixed safety limits: 4 MiB request and response bodies, 64 queued HTTP requests, 4096-character discovery queries, 100 commands per batch, 128 tasks per MCP session/client, and 1024 tasks globally. Async task list/get/cancel operations are isolated by `Mcp-Session-Id` (or the client identity fallback when no MCP session header is available).

The discovery catalog covers editor and asset operations, Blueprints, AI and navigation, animation, automation, configuration, conversations, Data Registry, Dataflow, Game Features, Gameplay Tags and GAS, Niagara, PCG, physics, plugins, semantic search, Slate, StateTree, UMG, World Conditions, and project-specific reflected APIs such as UnLua. Availability depends on the corresponding UE 5.7 or project plugin being enabled.
