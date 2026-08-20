# In-editor Streamable HTTP architecture

```mermaid
flowchart LR
    A["Codex / MCP client"] -->|"Streamable HTTP JSON-RPC"| W["ModelContextProtocol Editor module /mcp"]
    W -->|"Game Thread"| U["UE Python / Console / UObject APIs"]
    W -->|"Game Thread"| B["Native K2 Blueprint graph backend"]
```

## One server inside Unreal Editor

The `ModelContextProtocol` Editor module owns the MCP protocol server and serves the stateless Streamable HTTP endpoint directly from the Unreal Editor process. No child gateway process or external runtime is required. Clients connect to `http://127.0.0.1:18777/mcp` by default. Each project can select a different port under **Project Settings → Plugins → MCP for Unreal Editor**; `UE_MCP_PORT` remains a per-process override for automation.

The module implements JSON-RPC framing, MCP `server/discover`, legacy `initialize`, `ping`, `tools/list`, `tools/call`, local capability search, async task state, and optional bearer authentication. GET returns HTTP 405 because the stateless server has no standalone SSE notification stream; request responses are returned directly from POST as `application/json`.

HTTP requests are dispatched to the Game Thread before the module touches Python, `GEngine`, `GEditor`, or UObjects. The listener binds only to localhost, validates browser origins, and does not stop listeners owned by other UE modules during shutdown.

Blueprint graph commands use a narrow native backend modeled after UE 5.8's `BlueprintGraphEditor` and `BlueprintGraphPin` APIs. Function and variable nodes are created through `UBlueprintFunctionNodeSpawner` and `UBlueprintVariableNodeSpawner`; connections and defaults go through the graph's K2 schema. This is intentionally separate from broad Python execution because stock UE 5.7 does not expose the 5.8 graph editor API to Python.

## Protocol compatibility

The server supports the modern 2026-07-28 discovery handshake, including result envelopes and server identity metadata, plus legacy protocol versions from 2024-11-05 through 2025-11-25. Task lifecycle remains an action inside the single normal tool, so it does not depend on protocol-specific `tasks/*` methods.

## Distribution boundary

The Fab package contains one top-level `ModelContextProtocol` plugin with its descriptor, Source, Config, Content, Resources, license notices, and Editor binary. Node.js exists only as an optional development test client and is never included in the package.
