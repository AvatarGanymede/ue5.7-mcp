# UnrealMCP — native MCP for Unreal Engine 5.7

[English](README.md) | [简体中文](README.zh-CN.md)

## Introduction

UnrealMCP is a self-contained Unreal Engine 5.7 Editor Code Plugin for Win64. It embeds a Streamable HTTP MCP server directly in Unreal Editor, allowing Codex and other local MCP clients to inspect and control the open editor through exactly one MCP tool: `unreal`.

The plugin listens on `http://127.0.0.1:18777/mcp` by default. It runs UObject, Unreal Python, and console operations on the Game Thread and does not require a gateway executable, Node.js, npm, a Python package, or a separately installed runtime service.

Unreal Editor must remain open with the target project loaded while an MCP client is connected.

## Installation

### For Human

1. Download the named `UnrealMCP-...-UE5.7-Win64-GitHub.zip` asset from [GitHub Releases](https://github.com/AvatarGanymede/ue5.7-mcp/releases). Do not use GitHub's automatically generated **Source code** archives as the installer.
2. Close Unreal Editor. When upgrading, replace the complete existing `Plugins/UnrealMCP` directory instead of merging files into an older installation.
3. Extract the ZIP into the UE project root, next to the `.uproject` file. The final layout must be:

   ```text
   <Project>/
   ├─ <Project>.uproject
   └─ Plugins/
      └─ UnrealMCP/
         ├─ UnrealMCP.uplugin
         └─ Binaries/Win64/UnrealEditor-UnrealMCP.dll
   ```

4. Open the project, enable **Minimal MCP for Unreal Editor** and **Python Editor Script Plugin** under **Edit → Plugins**, then restart Unreal Editor.
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
4. Inspect the archive before extraction and verify that it contains `Plugins/UnrealMCP/UnrealMCP.uplugin` and `Plugins/UnrealMCP/Binaries/Win64/UnrealEditor-UnrealMCP.dll`.
5. Replace the complete `<Project>/Plugins/UnrealMCP` directory with the archive's plugin directory. Preserve unrelated plugins and project files.
6. Add or merge the following MCP configuration without overwriting unrelated Codex settings:

   ```toml
   [mcp_servers.unreal]
   url = "http://127.0.0.1:18777/mcp"
   tool_timeout_sec = 3600
   ```

7. Tell the user to enable **Minimal MCP for Unreal Editor** and **Python Editor Script Plugin**, then restart Unreal Editor and Codex.
8. After both applications restart, call:

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

The discovery catalog covers editor and asset operations, Blueprints, AI and navigation, animation, automation, configuration, conversations, Data Registry, Dataflow, Game Features, Gameplay Tags and GAS, Niagara, PCG, physics, plugins, semantic search, Slate, StateTree, UMG, World Conditions, and project-specific reflected APIs such as UnLua. Availability depends on the corresponding UE 5.7 or project plugin being enabled.
