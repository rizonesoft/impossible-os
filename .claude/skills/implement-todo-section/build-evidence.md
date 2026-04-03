# Build, Test, And Evidence Guide

## Default Build Path

- Use `bash scripts/build.sh` for the normal incremental build.
- Use `bash scripts/build.sh clean` only when stale-state symptoms justify it.
- Treat `tail -1 build/build.log` as the authoritative build result -- must show `=== BUILD OK ===`.

## Runtime Verification

- Use `bash scripts/build.sh run` for headless QEMU with serial output.
- Accept either terminal-visible serial output or captured serial logs as evidence.

## Crash And Fault Analysis

- Use `llvm-addr2line-19 -e build/kernel.exe -f <RIP>` for symbolized crash traces.
- Use `llvm-objdump-19` around the fault site for disassembly context.
- Use raw RIP speculation only after the symbolized path has been checked.

## Evidence To Leave Behind

- What changed in the scoped section.
- What was built, tested, or debugged.
- What passed, what failed, and any remaining limitation or follow-up.
