---
description: Build the OS from source and test in QEMU
---
> **Source:** [`.agents/workflows/build.md`](../../.agents/workflows/build.md)
> **Slash command:** `/build` (Antigravity), `/build` (Claude Code via `.claude/commands/`)

See the source file above for the full workflow. Quick reference:

1. Run `bash scripts/build.sh` for an incremental build
2. Check `tail -1 build/build.log` — must show `=== BUILD OK ===`
3. Run `bash scripts/build.sh run` to launch QEMU
4. Monitor serial output in `build/serial.log`
