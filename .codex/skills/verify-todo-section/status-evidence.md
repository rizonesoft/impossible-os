# Status And Evidence Rules

## Status Classification

| State | Use When | Action |
| ----- | -------- | ------ |
| `[x]` | Code, wiring, build, and required verification evidence all support the claim | Keep or mark complete |
| `[/]` | Some work exists, but the section is partial, blocked, or still missing acceptance criteria | Mark in progress and add blocker/missing-work notes |
| `[ ]` | Required work or evidence is missing | Leave pending or revert to pending |
| Mismatch | Implementation exists but TODO text is stale/inaccurate | Fix wording before reporting final state |

## Evidence Checklist

- Expected files, symbols, interfaces, and wiring paths exist and are non-stub.
- `build/build.log` supports claimed build status (`=== BUILD OK ===`).
- Runtime/serial evidence exists when behavior depends on execution.
- SSDT claims match service number/index, function, registration/dispatch, and table row.
- Cross-TODO closure claims include explicit `ID`/`SATISFIES` mapping and full acceptance-criteria proof.
- Remaining limits/follow-up work are called out explicitly.

## Conservative Update Rules

- Never mark `[x]` for placeholder or normal-path `STATUS_NOT_IMPLEMENTED` behavior.
- If proof is partial, keep `[ ]`/`[/]` and add missing unchecked prerequisite/ownership items in logical order.
- Preserve existing table/header formatting (including icons); update only evidence-backed cells/content.
