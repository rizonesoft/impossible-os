# Status And Evidence Rules

## Status Classification

| State | Use When | Action |
| ----- | -------- | ------ |
| `[x]` | Code, build, and required verification evidence all support the claim | Keep or mark complete |
| `[/]` | Some work exists, but the section is incomplete, partial, or blocked | Mark in progress and add a note |
| `[ ]` | Required work or evidence is missing | Leave pending or revert to pending |
| Mismatch | The implementation exists but the TODO text is stale or inaccurate | Fix the wording before reporting status |

## Evidence Checklist

- Expected files, symbols, or interfaces exist and are non-stub.
- Wiring/registration path exists where applicable (dispatch table, SSDT, hooks).
- `build/build.log` supports the claimed build status.
- Runtime or serial evidence exists when the section depends on execution behavior.
- SSDT claims match service/index, function, registration, and table row.
- Cross-TODO closure claims include explicit `ID`/`SATISFIES` mapping and full target-criteria coverage.
- Regressions or related subsystem checks are noted when relevant.
- Remaining limits or follow-up work are called out explicitly.

## Conservative Update Rules

- Do not mark work `[x]` if the implementation is a placeholder, stub, or unverified partial path.
- Do not mark work `[x]` for normal-path `STATUS_NOT_IMPLEMENTED`.
- Headless QEMU serial evidence counts the same as terminal-visible serial evidence when it is surfaced clearly.
- If names, paths, or notes drifted, correct the section text rather than forcing the code to match stale wording.
