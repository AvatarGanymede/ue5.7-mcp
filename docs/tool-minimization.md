# Tool-minimization loop

The stopping rule was: remove an MCP tool only if capability routing, execution, async control, error recovery, and protocol tests remain expressible without making the remaining schema ambiguous.

## Initial version: four tools

The straightforward design exposed `ue_discover`, `ue_execute`, `ue_task`, and `ue_health`. It was functionally clear but made every client carry four tool descriptions, and agents had to select between tools for every step.

## Loop 1: four to three

`health` was not a distinct capability; it was an execution preflight. It became an action in the common request envelope. Coverage was unchanged.

## Loop 2: three to two

The official 5.8 design already demonstrates that a meta-call can select a dynamically described operation. Discovery and execution therefore share one `unreal` entry with an `action` discriminator. This removed cross-tool routing while preserving search and schemas at the capability-domain level.

## Loop 3: two to one

Async task lifecycle was folded into the same entry as `action=task`. This is also protocol-resilient: the 2026-07-28 MCP era removes the old `tasks/*` methods, while a normal tool action works in both eras. Native stdio integration tests assert a one-item tool list in both eras.

## Loop 4: can one become zero?

No. Resources or prompts can describe Unreal, but without at least one callable tool the agent cannot cause an editor action. Encoding execution as a resource read would violate MCP semantics and make side effects harder to reason about. The loop stops at **one tool**.

## Loop 5: minimize schema ambiguity, not the tool count

A final adversarial pass kept the one-tool surface but replaced a flat envelope of mostly optional fields with an `action`-discriminated union stored in the plugin metadata. Clients now see that `execute.commands` and `task.command` are required and which fields belong to each lifecycle phase. Splitting read and write actions back into separate tools would improve annotation granularity, but would violate the explicit minimum-surface objective; zero tools remains non-functional.

## Final surface

`unreal(action, ...)` is the irreducible MCP surface. The tool's internal action discriminator is not hidden complexity: it separates four lifecycle phases in one stable schema while capability-specific APIs remain searchable data rather than globally exposed tools.

The tradeoff is annotation granularity. MCP annotations apply to the whole tool, so the umbrella tool is conservatively marked potentially destructive even for read-only discovery. That is preferable to falsely marking mixed operations read-only. If a host requires per-operation approval semantics, a two-tool profile (`unreal_read`, `unreal_write`) would be safer but no longer minimal.
