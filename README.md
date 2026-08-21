# UnrealMCP — Native MCP for Unreal Engine 5.7

[English](README.md) | [简体中文](README.zh-CN.md)

UnrealMCP is a Win64 editor plugin that embeds a Streamable HTTP MCP server in Unreal Engine 5.7. Codex and other local MCP clients can inspect and control the open editor through one tool: `unreal`.

- Default endpoint: `http://127.0.0.1:18777/mcp`
- Runs UObject, Unreal Python, console, and Blueprint graph operations on the Game Thread
- Requires no gateway, Node.js, npm, or separate runtime service
- Requires Unreal Editor to remain open with the target project loaded

## Install

1. Download `UnrealMCP-...-UE5.7-Win64-GitHub.zip` from [GitHub Releases](https://github.com/AvatarGanymede/ue5.7-mcp/releases). Do not use GitHub's automatic **Source code** archives.
2. Close Unreal Editor. Extract the ZIP beside the project's `.uproject` file so the plugin is installed at `Plugins/ModelContextProtocol`.
3. When upgrading, replace that directory completely. For versions 0.3.x and earlier, also remove the legacy `Plugins/UnrealMCP` directory.
4. In **Edit → Plugins**, enable **MCP for Unreal Editor** and **Python Editor Script Plugin**, then restart Unreal Editor.
5. Add the server to Codex:

   ```bash
   codex mcp add unreal --url http://127.0.0.1:18777/mcp
   ```

   Or add it to the trusted project's `.codex/config.toml`:

   ```toml
   [mcp_servers.unreal]
   url = "http://127.0.0.1:18777/mcp"
   tool_timeout_sec = 3600
   ```

6. Restart or reconnect Codex, then call:

   ```json
   { "action": "health" }
   ```

A ready server reports `ok: true`, `is_game_thread: true`, and `python_loaded: true`.

> If you enable the plugin directly in `.uproject`, include `"SupportedTargetPlatforms": ["Win64"]` in its plugin entry.

## Configuration

The server listens on `127.0.0.1:18777` by default. To run multiple Unreal projects at once, assign each project a different port under **Edit → Project Settings → Plugins → MCP for Unreal Editor**, restart Unreal Editor, and update the matching MCP URL.

`UE_MCP_PORT` can temporarily override the project setting when set before Unreal Editor starts.

## Use

The server exposes one MCP tool named `unreal` with four actions:

| Action | Purpose |
|---|---|
| `health` | Check server, engine, Python, thread, and endpoint status. |
| `discover` | Find supported workflows, UE APIs, and plugin mount status. |
| `execute` | Run ordered Python, console, Blueprint graph, or wait commands. |
| `task` | Get, list, or cancel asynchronous work. |

Discover a workflow before choosing UE APIs:

```json
{
  "action": "discover",
  "query": "create and compile a blueprint",
  "limit": 5
}
```

Run an ordered batch:

```json
{
  "action": "execute",
  "run": "sync",
  "transaction": true,
  "commands": [
    {
      "kind": "python",
      "mode": "eval",
      "code": "unreal.SystemLibrary.get_engine_version()"
    },
    {
      "kind": "console",
      "command": "stat fps"
    }
  ]
}
```

For long-running batches, use `"run": "async"` and query the returned `task_id`:

```json
{ "action": "task", "command": "get", "task_id": "<uuid>" }
```

Use Python and `BlueprintEditorLibrary` for Blueprint assets, variables, components, and class defaults. Use `kind: "blueprint_graph"` for visible K2 nodes and connections; start with `operation: "inspect"` to obtain stable node and pin references. Timeline/track authoring is not currently supported.

Wait commands require async mode and never block the Game Thread. Transactions create Undo entries but are not atomic; failures, timeouts, and cancellation do not automatically roll back earlier changes.

For direct diagnostics from Git Bash:

```bash
scripts/unreal-mcp.sh '{"action":"health"}'
scripts/unreal-mcp.sh --list
```

## Build from source

From Git Bash on Windows:

```bash
scripts/build-plugin.sh --engine-root 'C:/Program Files/Epic Games/UE_5.7'
```

The packaged plugin is written under `artifacts/`. Use `--output PATH` to choose a new output directory.

## More detail

- [Architecture](docs/architecture.md)
- [Capability coverage](docs/capability-coverage.md)
- [Tool minimization](docs/tool-minimization.md)
