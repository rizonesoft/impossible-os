# Build, Test, And Evidence Guide

## Default Build Path

- Use `bash scripts/build.sh` for the normal incremental build.
- Use `bash scripts/build.sh clean` only when the scope, generated artifacts, or stale-state symptoms justify it.
- Treat `tail -1 build/build.log` as the authoritative build result.

## Runtime Verification

- Prefer headless QEMU with serial output for autonomous loops and reproducible evidence.
- Accept either terminal-visible serial output or captured serial logs when the script surfaces actionable evidence.
- Do not assume a GUI-only QEMU path.

## Crash And Fault Analysis

- If symbols are available, use:
  - `llvm-addr2line-19 -e build/kernel.exe -f <RIP>`
  - `llvm-objdump-19` around the fault site
- Use raw RIP speculation only after the symbolized path has been checked.

## Commit-Time Expectations

- If repo `.githooks/` are installed, expect staged C or H linting in `pre-commit`.
- Also expect `post-commit` to regenerate `COUNT.md` and potentially amend the commit.

## Evidence To Leave Behind

- What changed in the scoped section.
- What was built, tested, or debugged.
- What passed, what failed, and any remaining limitation or follow-up.
