# UnrealMCP — 面向 Unreal Engine 5.7 的原生 MCP

[English](README.md) | [简体中文](README.zh-CN.md)

## 简介

UnrealMCP 是一个面向 Win64 的自包含 Unreal Engine 5.7 编辑器代码插件。它把 Streamable HTTP MCP 服务直接嵌入 Unreal Editor，让 Codex 和其他本地 MCP 客户端通过唯一的 MCP 工具 `unreal` 检查并控制已打开的编辑器。

插件默认监听 `http://127.0.0.1:18777/mcp`。UObject、Unreal Python 和控制台操作都会在游戏线程执行；产品运行时不需要网关 EXE、Node.js、npm、Python 第三方包或单独安装的服务。

MCP 客户端连接期间，必须保持 Unreal Editor 已启动并打开目标工程。

## 安装说明

### For Human

1. 从 [GitHub Releases](https://github.com/AvatarGanymede/ue5.7-mcp/releases) 下载命名为 `UnrealMCP-...-UE5.7-Win64-GitHub.zip` 的附件。不要把 GitHub 自动生成的 **Source code** 压缩包当作安装包。
2. 关闭 Unreal Editor。升级时应完整替换已有的 `Plugins/ModelContextProtocol` 目录，不要把新文件合并覆盖到旧安装中。从 0.3.x 或更早版本升级时，删除旧的 `Plugins/UnrealMCP` 目录；不要同时保留两个插件标识。
3. 将 ZIP 解压到 UE 工程根目录，也就是 `.uproject` 文件旁边。最终结构必须是：

   ```text
   <Project>/
   ├─ <Project>.uproject
   └─ Plugins/
      └─ ModelContextProtocol/
         ├─ ModelContextProtocol.uplugin
         └─ Binaries/Win64/UnrealEditor-ModelContextProtocol.dll
   ```

4. 打开工程，在 **Edit → Plugins** 中启用 **MCP for Unreal Editor** 和 **Python Editor Script Plugin**，然后重启 Unreal Editor。
5. 将 MCP 端点添加到 Codex：

   ```powershell
   codex mcp add unreal --url http://127.0.0.1:18777/mcp
   ```

   也可以写入 `~/.codex/config.toml`，或可信工程内的 `.codex/config.toml`：

   ```toml
   [mcp_servers.unreal]
   url = "http://127.0.0.1:18777/mcp"
   tool_timeout_sec = 3600
   ```

6. 重启 Codex，确认 `unreal` 已连接，然后调用 `health` 动作。

### For Agent

为用户安装 UnrealMCP 时，按照以下流程执行：

1. 查找 `.uproject` 文件以确定目标工程根目录。除非用户明确要求安装到引擎，否则使用工程级安装。
2. 获取最新的命名发布附件 `UnrealMCP-*-UE5.7-Win64-GitHub.zip`，绝不能用 GitHub 自动生成的源码压缩包代替。
3. 替换二进制文件前，让用户关闭 Unreal Editor。不要覆盖正在使用的插件。
4. 解压前检查压缩包，确认包含 `Plugins/ModelContextProtocol/ModelContextProtocol.uplugin` 和 `Plugins/ModelContextProtocol/Binaries/Win64/UnrealEditor-ModelContextProtocol.dll`。
5. 用压缩包中的插件目录完整替换 `<Project>/Plugins/ModelContextProtocol`。从 0.3.x 或更早版本迁移时，删除 `<Project>/Plugins/UnrealMCP`，避免新旧插件标识同时加载。保留其他插件和工程文件不变。
6. 按当前 agent 自身的 MCP 配置规范，将名为 `unreal`、URL 为 `http://127.0.0.1:18777/mcp` 的 Streamable HTTP 服务注册到该 agent 的 **project scope MCP list**。不要注册到 user scope 或 global scope，也不要覆盖工程内其他 MCP 条目。如果该 agent 支持工具超时设置，将其设为 3600 秒。
7. 告知用户启用 **MCP for Unreal Editor** 和 **Python Editor Script Plugin**，然后重启 Unreal Editor。
8. Unreal Editor 重启后调用：

   ```json
   { "action": "health" }
   ```

   就绪的服务应返回 `ok: true`、`is_game_thread: true` 和 `python_loaded: true`。
9. 验证 `tools/list` 暴露了 `unreal`，并确认 host 将它显示为可直接调用的工具。如果 host 没有刷新 project MCP 工具，请重连或重启 host。需要从 Git Bash 排查时，可运行 `scripts/unreal-mcp.sh --list`；这个辅助脚本只用于避免手写 JSON-RPC 信封，并非产品运行时依赖。

## MCP 提供的工具集

服务只暴露一个名为 `unreal` 的 MCP 工具，通过 `action` 字段选择四类操作：

| Action | 用途 |
|---|---|
| `health` | 检查服务状态、引擎版本、游戏线程调度、Python 可用性、传输方式和端点。 |
| `discover` | 搜索能力域、可运行的 UE 5.7 API 起点，以及已发现插件的挂载状态。 |
| `execute` | 执行最多 100 条有序 Python、控制台或异步非阻塞等待命令。 |
| `task` | 查询、列出或取消通过 `execute` 提交的异步任务。 |

检查连接：

```json
{ "action": "health" }
```

如果 MCP host 尚未直接显示 `unreal`，可以从 Git Bash 用仓库辅助脚本调用同一端点：

```bash
scripts/unreal-mcp.sh '{"action":"health"}'
scripts/unreal-mcp.sh --list
```

选择 UE API 前先发现相关工作流：

```json
{
  "action": "discover",
  "query": "create and compile a blueprint",
  "limit": 5
}
```

在游戏线程执行有序批处理：

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

长耗时操作可设置 `"run": "async"`，响应会返回 `task_id`。查询方式：

```json
{ "action": "task", "command": "get", "task_id": "<uuid>" }
```

必须观察后续 Tick 的运行时断言可以使用非阻塞等待。等待命令要求 `run=async`；同步调用会被拒绝，不会用 sleep 阻塞游戏线程：

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

异步批处理每个游戏线程 tick 执行一条命令。300 秒墙钟时间限制和协作式取消只在命令之间检查；正在执行的 Python 或控制台命令不会被中断。`transaction: true` 会为每条 Python 和控制台命令分别记录 Undo；`wait` 不会添加 Undo。服务不会假设 Python `eval` 是只读操作，因为表达式仍然可以调用会修改 UObject 的方法。事务不具备原子性，Python 也可能在抛异常前已经修改 UObject。失败、超时或取消不会自动回滚；应检查 `partial_changes_possible`、`commands_completed`、`commands_succeeded` 和 `failed_command_index`，必要时进行幂等清理。Python 失败的完整 traceback 会同时出现在命令级 `result` 和 `error` 中。

服务端会依据发布的 JSON Schema 校验工具参数，并强制执行固定安全上限：请求和响应各 4 MiB、HTTP 待处理队列 64 个请求、发现查询 4096 个字符、每批 100 条命令、每个 MCP 会话或客户端 128 个任务、全局 1024 个任务。异步任务的 list/get/cancel 按 `Mcp-Session-Id` 隔离；没有 MCP 会话头时使用客户端身份回退值。

能力目录覆盖编辑器与资产操作、Blueprint、AI 与导航、动画、自动化、配置、对话、Data Registry、Dataflow、Game Features、Gameplay Tags 与 GAS、Niagara、PCG、物理、插件、语义搜索、Slate、StateTree、UMG、World Conditions，以及 UnLua 等工程专用反射 API。对 PIE、碰撞、当前动画、Player Pawn 和 Viewport Widget 等高频易错调用，目录会返回可运行 recipe。按插件名称查询时，还会报告已发现的启用或禁用插件及其 `enabled`、`mounted`、`can_contain_content`、`content_dir` 和 `mounted_asset_path`，用来诊断 Asset Registry 在插件内容挂载前无法看到的资产；服务不会递归索引禁用插件目录中的文件。具体能力是否可用取决于相应的 UE 5.7 或工程插件是否已经启用。
