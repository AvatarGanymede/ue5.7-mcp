# UnrealMCP — 面向 Unreal Engine 5.7 的原生 MCP

[English](README.md) | [简体中文](README.zh-CN.md)

## 简介

UnrealMCP 是一个面向 Win64 的自包含 Unreal Engine 5.7 编辑器代码插件。它把 Streamable HTTP MCP 服务直接嵌入 Unreal Editor，让 Codex 和其他本地 MCP 客户端通过唯一的 MCP 工具 `unreal` 检查并控制已打开的编辑器。

插件默认监听 `http://127.0.0.1:18777/mcp`。UObject、Unreal Python 和控制台操作都会在游戏线程执行；产品运行时不需要网关 EXE、Node.js、npm、Python 第三方包或单独安装的服务。

MCP 客户端连接期间，必须保持 Unreal Editor 已启动并打开目标工程。

## 安装说明

### For Human

1. 从 [GitHub Releases](https://github.com/AvatarGanymede/ue5.7-mcp/releases) 下载命名为 `UnrealMCP-...-UE5.7-Win64-GitHub.zip` 的附件。不要把 GitHub 自动生成的 **Source code** 压缩包当作安装包。
2. 关闭 Unreal Editor。升级时应完整替换已有的 `Plugins/UnrealMCP` 目录，不要把新文件合并覆盖到旧安装中。
3. 将 ZIP 解压到 UE 工程根目录，也就是 `.uproject` 文件旁边。最终结构必须是：

   ```text
   <Project>/
   ├─ <Project>.uproject
   └─ Plugins/
      └─ UnrealMCP/
         ├─ UnrealMCP.uplugin
         └─ Binaries/Win64/UnrealEditor-UnrealMCP.dll
   ```

4. 打开工程，在 **Edit → Plugins** 中启用 **Minimal MCP for Unreal Editor** 和 **Python Editor Script Plugin**，然后重启 Unreal Editor。
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
4. 解压前检查压缩包，确认包含 `Plugins/UnrealMCP/UnrealMCP.uplugin` 和 `Plugins/UnrealMCP/Binaries/Win64/UnrealEditor-UnrealMCP.dll`。
5. 用压缩包中的插件目录完整替换 `<Project>/Plugins/UnrealMCP`。保留其他插件和工程文件不变。
6. 添加或合并以下 MCP 配置，不要覆盖无关的 Codex 设置：

   ```toml
   [mcp_servers.unreal]
   url = "http://127.0.0.1:18777/mcp"
   tool_timeout_sec = 3600
   ```

7. 告知用户启用 **Minimal MCP for Unreal Editor** 和 **Python Editor Script Plugin**，然后重启 Unreal Editor 和 Codex。
8. 两个应用重启后调用：

   ```json
   { "action": "health" }
   ```

   就绪的服务应返回 `ok: true`、`is_game_thread: true` 和 `python_loaded: true`。

## MCP 提供的工具集

服务只暴露一个名为 `unreal` 的 MCP 工具，通过 `action` 字段选择四类操作：

| Action | 用途 |
|---|---|
| `health` | 检查服务状态、引擎版本、游戏线程调度、Python 可用性、传输方式和端点。 |
| `discover` | 搜索能力目录，查找相关 Unreal API、子系统、控制台命令和工作流示例。 |
| `execute` | 同步或异步执行最多 100 条有序的 Unreal Python 或控制台命令。 |
| `task` | 查询、列出或取消通过 `execute` 提交的异步任务。 |

检查连接：

```json
{ "action": "health" }
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

能力目录覆盖编辑器与资产操作、Blueprint、AI 与导航、动画、自动化、配置、对话、Data Registry、Dataflow、Game Features、Gameplay Tags 与 GAS、Niagara、PCG、物理、插件、语义搜索、Slate、StateTree、UMG、World Conditions，以及 UnLua 等工程专用反射 API。具体能力是否可用取决于相应的 UE 5.7 或工程插件是否已经启用。
