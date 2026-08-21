# `unreal` tool call reference

Use these envelopes as patterns. The MCP tool schema is strict: omit unknown fields rather than adding explanatory properties.

## Health and discovery

```json
{"action":"health"}
```

```json
{
  "action": "discover",
  "query": "create and compile a Blueprint actor",
  "limit": 5
}
```

Use `domain` only when its exact identifier is already known. Useful domains include `actor-world`, `asset`, `blueprint`, `editor-ui`, `automation`, `plugins`, `pcg`, `niagara`, `umg`, and `state-tree`.

## Python

Read one value:

```json
{
  "action": "execute",
  "transaction": false,
  "commands": [{
    "kind": "python",
    "mode": "eval",
    "code": "__import__('unreal').SystemLibrary.get_engine_version()",
    "label": "read engine version"
  }]
}
```

Perform a checked mutation:

```json
{
  "action": "execute",
  "transaction": true,
  "commands": [{
    "kind": "python",
    "mode": "exec",
    "code": "import unreal\nasset = unreal.load_asset('/Game/Blueprints/BP_Example')\nassert asset is not None, 'BP_Example was not found'\nunreal.BlueprintEditorLibrary.compile_blueprint(asset)",
    "label": "compile BP_Example"
  }]
}
```

Call a reusable project extension from `Content/Python`:

```json
{
  "action": "execute",
  "transaction": true,
  "commands": [{
    "kind": "python",
    "mode": "exec",
    "code": "import project_tools\nproject_tools.build_level('Arena01')",
    "label": "run project level builder"
  }]
}
```

Import the module in each command because Python command scopes are private. Use `importlib.reload(project_tools)` only while iterating on the module, then verify through its documented query function or a separate UE state inspection.

## Console

```json
{
  "action": "execute",
  "transaction": false,
  "commands": [{"kind":"console","command":"stat fps"}]
}
```

## Blueprint graph

Start by inspecting the Blueprint object path:

```json
{
  "action": "execute",
  "transaction": false,
  "commands": [{
    "kind": "blueprint_graph",
    "operation": "inspect",
    "blueprint_path": "/Game/Blueprints/BP_Example.BP_Example"
  }]
}
```

Available operations are `inspect`, `add_event`, `add_custom_event`, `add_function_call`, `add_variable_get`, `add_variable_set`, `connect`, `disconnect`, `set_pin_default`, `remove_node`, `move_node`, `set_comment`, and `compile`.

Use identifiers returned by `inspect` or node creation. For example, connect compatible pins with:

```json
{
  "action": "execute",
  "transaction": true,
  "commands": [{
    "kind": "blueprint_graph",
    "operation": "connect",
    "blueprint_path": "/Game/Blueprints/BP_Example.BP_Example",
    "from_pin_id": "<output-pin-uuid>",
    "to_pin_id": "<input-pin-uuid>"
  }]
}
```

Compile and optionally save:

```json
{
  "action": "execute",
  "transaction": true,
  "commands": [{
    "kind": "blueprint_graph",
    "operation": "compile",
    "blueprint_path": "/Game/Blueprints/BP_Example.BP_Example",
    "save": true
  }]
}
```

## Asynchronous execution

```json
{
  "action": "execute",
  "run": "async",
  "transaction": true,
  "commands": [
    {"kind":"python","mode":"exec","code":"import project_tools\nproject_tools.start_build()"},
    {"kind":"wait","frames":2,"label":"yield for editor ticks"},
    {"kind":"python","mode":"eval","code":"__import__('project_tools').build_is_ready()"}
  ]
}
```

Read the created task identifier from `data.task.id`, then poll it as `task_id`:

```json
{"action":"task","command":"get","task_id":"<task-uuid>"}
```

Continue while `data.task.state` is `running`. Terminal states are `succeeded`, `failed`, `timed_out`, and `cancelled`; inspect `data.task.result` or `data.task.error` before reporting completion.

List owned tasks or cancel one:

```json
{"action":"task","command":"list"}
```

```json
{"action":"task","command":"cancel","task_id":"<task-uuid>"}
```

Wait supports either `frames` from 1 to 18000 or `seconds` from 0.001 to 299. A wait command is invalid in synchronous mode. Every synchronous or asynchronous batch has a 300-second total execution limit.
