# UE 5.8 capability coverage

Baseline inspected: Epic `UnrealEngine` branch `5.8`, commit `265a0946fedc02a002f77682b402b336b8208ac1`. The inspection was read-only and used only to identify plugin and callable names; no Epic implementation is included here.

UE 5.8's `AllToolsets.uplugin` enables 21 Toolset plugins. The independent catalog maps all 21 to one or more capability domains:

| Official plugin group | UnrealMCP domain |
|---|---|
| AIModuleToolset | `ai-navigation` |
| AnimationAssistantToolset | `animation` |
| AutomationTestToolset | `automation` |
| ConfigSettingsToolset | `config` |
| ConversationToolset | `conversation` |
| DataRegistryToolset | `data-registry` |
| DataflowAgent | `dataflow` |
| EditorToolset | `actor-world`, `asset`, `blueprint`, `editor-ui`, `logs-cvars` |
| GameFeaturesToolset | `game-features` |
| GameplayTagsToolset | `gameplay-tags` |
| GASToolsets | `gas` |
| MCPClientToolset | `mcp-client` |
| NiagaraToolsets | `niagara` |
| PCGToolset | `pcg` |
| PhysicsToolsets | `physics` |
| PluginToolset | `plugins` |
| SemanticSearchToolset | `semantic-search` |
| SlateInspectorToolset | `slate`, `editor-ui` |
| StateTreeToolset | `state-tree` |
| UMGToolSet | `umg` |
| WorldConditionsToolset | `world-conditions` |

The UE 5.8 source tree also contains adjacent optional Toolsets. `cloth`, `live-coding`, `metahuman`, `umg`/MVVM, and `animation`/Sequencer domains cover those APIs when the corresponding UE 5.7 plugin exists.

The extracted 5.8 baseline contained at least 274 reflected `AICallable` operations across 19 directly callable plugin modules. This implementation does not reproduce 274 wrappers. It routes through UE 5.7 Python/reflection and console APIs, which form a capability superset and also reach project-specific systems such as UnLua.

Coverage has three meanings:

- **Routing coverage:** every official aggregate plugin has a discoverable domain and preferred backend. Enforced against the native metadata by `tests/metadata.test.ts`.
- **Mechanism coverage:** arbitrary UE Python/reflection, console calls, batches, captures/log output, transactions, and long-call tracking can express the official workflows.
- **Version availability:** a UE 5.8-only subsystem cannot exist in stock UE 5.7. The tool still exposes the equivalent workflow when 5.7 has the subsystem/plugin; otherwise it returns the engine's normal missing-class/plugin error rather than pretending the API exists.

For a production project, add execution tests for the exact enabled plugins and representative assets. Static routing coverage is necessary but cannot prove that optional engine plugins are installed or that a project asset is valid.
