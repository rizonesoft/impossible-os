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
- `build/build.log` supports the claimed build status.
- Runtime or serial evidence exists when the section depends on execution behavior.
- Regressions or related subsystem checks are noted when relevant.
- Remaining limits or follow-up work are called out explicitly.

## Conservative Update Rules

- Do not mark work `[x]` if the implementation is a placeholder, stub, or unverified partial path.
- Headless QEMU serial evidence counts the same as terminal-visible serial evidence when surfaced clearly.
- If names, paths, or notes drifted, correct the section text rather than forcing the code to match stale wording.
