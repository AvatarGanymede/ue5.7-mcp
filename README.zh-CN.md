# UnrealMCP — 面向 Unreal Engine 5.7 的原生 MCP

[English](README.md) | [简体中文](README.zh-CN.md)

![Agent-MCP-Engine 数据沟通协议与执行链路](imgs/agent-mcp-engine-protocol.zh-CN.drawio.png)

UnrealMCP 是一个 Win64 编辑器插件，将 Streamable HTTP MCP 服务直接嵌入 Unreal Engine 5.7。Codex 和其他本地 MCP 客户端可通过唯一的 `unreal` 工具检查并控制已打开的编辑器。

- 默认端点：`http://127.0.0.1:18777/mcp`
- UObject、Unreal Python、控制台和 Blueprint 图操作均在游戏线程执行
- 不需要网关、Node.js、npm 或独立运行时服务
- 使用期间必须保持 Unreal Editor 已启动并打开目标工程

## 安装

1. 从 [GitHub Releases](https://github.com/AvatarGanymede/ue5.7-mcp/releases) 下载 `UnrealMCP-...-UE5.7-Win64-GitHub.zip`。不要使用 GitHub 自动生成的 **Source code** 压缩包。
2. 关闭 Unreal Editor。将 ZIP 解压到工程的 `.uproject` 文件旁，确保插件位于 `Plugins/ModelContextProtocol`。
3. 升级时完整替换该目录。从 0.3.x 或更早版本升级时，还需删除旧的 `Plugins/UnrealMCP` 目录。
4. 在 **Edit → Plugins** 中启用 **MCP for Unreal Editor** 和 **Python Editor Script Plugin**，然后重启 Unreal Editor。
5. 将服务添加到 Codex：

   ```bash
   codex mcp add unreal --url http://127.0.0.1:18777/mcp
   ```

   也可写入可信工程的 `.codex/config.toml`：

   ```toml
   [mcp_servers.unreal]
   url = "http://127.0.0.1:18777/mcp"
   tool_timeout_sec = 3600
   ```

6. 重启或重新连接 Codex，然后调用：

   ```json
   { "action": "health" }
   ```

服务就绪时会返回 `ok: true`、`is_game_thread: true` 和 `python_loaded: true`。

> 如果直接在 `.uproject` 中启用插件，请在插件条目中加入 `"SupportedTargetPlatforms": ["Win64"]`。

## 配置

服务默认监听 `127.0.0.1:18777`。如需同时打开多个 Unreal 工程，请在 **Edit → Project Settings → Plugins → MCP for Unreal Editor** 中为每个工程分配不同端口，重启 Unreal Editor，并同步修改对应的 MCP URL。

在启动 Unreal Editor 前设置 `UE_MCP_PORT`，可临时覆盖工程端口。

## 使用

服务只暴露一个名为 `unreal` 的 MCP 工具，包含四种动作：

| Action | 用途 |
|---|---|
| `health` | 检查服务、引擎、Python、线程和端点状态。 |
| `discover` | 查找支持的工作流、UE API 和插件挂载状态。 |
| `execute` | 执行有序的 Python、控制台、Blueprint 图或等待命令。 |
| `task` | 查询、列出或取消异步任务。 |

选择 UE API 前先发现相关工作流：

```json
{
  "action": "discover",
  "query": "create and compile a blueprint",
  "limit": 5
}
```

执行有序批处理：

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

长耗时批处理可使用 `"run": "async"`，再查询返回的 `task_id`：

```json
{ "action": "task", "command": "get", "task_id": "<uuid>" }
```

Blueprint 资产、变量、组件和类默认值使用 Python 与 `BlueprintEditorLibrary`；可见 K2 节点和连线使用 `kind: "blueprint_graph"`。建议先执行 `operation: "inspect"` 获取稳定的节点和引脚引用。当前不支持 Timeline/Track 编写。

等待命令只能用于异步模式，不会阻塞游戏线程。事务会创建 Undo 记录，但不具备原子性；失败、超时或取消不会自动回滚先前的修改。

需要从 Git Bash 直接排查时：

```bash
scripts/unreal-mcp.sh '{"action":"health"}'
scripts/unreal-mcp.sh --list
```

## 从源码构建

在 Windows 的 Git Bash 中运行：

```bash
scripts/build-plugin.sh --engine-root 'C:/Program Files/Epic Games/UE_5.7'
```

插件包会输出到 `artifacts/`。可用 `--output PATH` 指定一个新的输出目录。

## 更多资料

- [架构](docs/architecture.md)
- [能力覆盖](docs/capability-coverage.md)
- [工具精简设计](docs/tool-minimization.md)
