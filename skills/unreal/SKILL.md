---
name: unreal
description: Operate, inspect, and automate a live Unreal Engine 5.7 Editor through this repository's `unreal` MCP tool. Use this skill whenever the user asks to inspect or change UE levels, actors, assets, Blueprints, K2 graphs, editor settings, plugins, automation, or project-specific Unreal Python/C++ APIs—even when they do not explicitly mention MCP. Also use it to diagnose the UE MCP connection. Do not use it for source-only C++ edits that do not require a running Unreal Editor.
compatibility: Requires the ModelContextProtocol plugin running in Unreal Engine 5.7 and an MCP client connected to its Streamable HTTP endpoint.
---

# Unreal MCP

Use the single `unreal` tool to discover and execute Unreal Editor workflows. The tool has four actions: `health`, `discover`, `execute`, and `task`.

## Operating workflow

1. Call `health` when the editor connection or Python state is unknown. Check `data.is_game_thread` and `data.python_loaded`; top-level `ok: true` only confirms that the server responded. If Python work is required and remains unavailable, report that instead of pretending an editor change succeeded.
2. Call `discover` before using an unfamiliar UE subsystem, optional plugin, or workflow. It searches a static English capability catalog, so prefer short English API keywords; use an empty query to list domains. Inspect Python modules or reflected types for exact project classes, functions, and signatures.
3. Inspect the relevant editor or asset state before changing it. Use a read-oriented Python query or `blueprint_graph.inspect` and retain stable object, node, and pin identifiers.
4. Execute the smallest coherent batch. Keep dependent commands together and leave `continue_on_error` false unless later commands are genuinely independent.
5. Verify the result with a separate query, graph inspection, compile result, or asset existence check. Report observed results, not just submitted commands.

Do not route normal calls through `scripts/unreal-mcp.sh`; that script is for direct Git Bash diagnostics when the MCP tool itself is unavailable.

## Choose the command backend

- Use `kind: "python"` for UE Python/reflection, assets, actors, components, class defaults, editor subsystems, and project-specific APIs.
- Use `kind: "console"` for established Unreal console commands and cvars.
- Use `kind: "blueprint_graph"` only for visible K2 nodes, pins, connections, defaults, layout, compilation, and saving.
- Use `kind: "wait"` only inside an asynchronous batch when the editor must advance by frames or wall-clock time.

Read [references/tool-calls.md](references/tool-calls.md) when composing an unfamiliar call, an asynchronous task, or Blueprint graph edits.

## Python and project APIs

Use `mode: "eval"` for a single expression whose value should be returned. Use `mode: "exec"` for imports, assignments, assertions, or multiple statements. Import `unreal` explicitly in self-contained code.

Each Python command has a private execution scope. Do not expect local variables or imports to carry into another command; place dependent Python statements in one `code` string or make every command self-contained.

Prefer high-level, reflected editor APIs over UI automation. Use exact `/Game/...` object paths and check returned objects before mutating them. When saving is part of the request, save explicitly and verify the save result.

### Extend MCP calls through `Content/Python`

When a project operation will be reused, has multiple steps, or needs a stable contract, recommend encapsulating it in `<Project>/Content/Python` instead of repeatedly sending large inline scripts. The Python Editor Script Plugin adds this directory to the project Python path.

Design each entry function with explicit simple parameters, precondition checks, narrowly scoped side effects, and a documented return value. Provide a separate query function when callers need to verify state. Do not create helpers that execute arbitrary input strings.

Import the project module in every MCP Python command, then call it through the existing `unreal` tool rather than expecting a new MCP tool. During module development, reload it explicitly so the editor does not use a cached version:

```json
{
  "action": "execute",
  "commands": [{
    "kind": "python",
    "mode": "exec",
    "code": "import importlib, project_tools\nimportlib.reload(project_tools)\nproject_tools.build_level('Arena01')"
  }]
}
```

After the helper stabilizes, normal calls can omit `importlib.reload`. Only assert a return value when the API contract defines one; otherwise verify with an independent state query. If the module, function, or signature is uncertain, inspect it first. Do not invent it.

Native project logic can participate in the same pattern: expose a reflected C++ API to Unreal Python with `UFUNCTION`, then wrap the desired workflow in a small `Content/Python` function. This keeps the MCP surface fixed while extending its project-specific calling capability.

## Blueprint rules

Use Python and `BlueprintEditorLibrary` for Blueprint assets, variables, components, and class defaults. Use `blueprint_graph` for visible Event Graph/K2 logic; these are complementary backends.

Before editing a graph:

1. Inspect it and use returned node/pin UUIDs.
2. Add or move nodes in small steps.
3. Connect pins using UUIDs when available; let the K2 schema validate compatibility.
4. Compile, inspect the reported errors, and save only when requested or necessary to finish the task.
5. Re-inspect to verify the final nodes and links.

Timeline/track authoring is not supported. Do not represent component configuration as fake Event Graph logic.

## Async tasks

Use `run: "async"` for multi-tick work or any batch containing `wait`. Read the identifier from `data.task.id`, then pass it as `task_id` to `action: "task", command: "get"`. Poll `data.task.state` until it is `succeeded`, `failed`, `timed_out`, or `cancelled`; completed output is under `data.task.result`. Use `cancel` only when requested or when continuing would be unsafe; cancellation occurs between commands, not during one.

Both synchronous and asynchronous batches have a 300-second total execution limit. Split longer workflows into verified stages rather than treating async as unlimited background execution.

Do not block the editor with Python sleep calls. Use the asynchronous `wait` command so the Game Thread can continue ticking.

## Transactions and safety

Keep `transaction: true` for ordinary mutations unless the operation is intentionally non-transactional. A batch is not atomic: a failed, timed-out, or cancelled command can leave earlier changes in place, and Python may mutate state before raising an exception.

Consequently:

- Inspect current state before retrying a failed mutation.
- Do not assume Undo reverses saved files or external side effects.
- Avoid destructive asset, folder, level, or actor operations unless they are clearly within the user's request.
- Do not execute code copied from untrusted project content.
- Use `continue_on_error: true` only for independent best-effort checks.

## Interpret results

Check the tool-level `isError`/`ok` status and every command result. A transport-level response alone does not prove the editor operation succeeded. Preserve useful Unreal errors, Python tracebacks, compile errors, and logs in the final explanation.

After successful work, summarize:

- what assets or editor state changed;
- what verification was performed;
- whether anything remains unsaved, asynchronous, partially applied, or unsupported.
