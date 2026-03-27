---
description: Build the OS from source and test in QEMU
---
Run: `bash scripts/build.sh` → verify `tail -1 build/build.log` shows `=== BUILD OK ===`
Then: `bash scripts/build.sh run` to launch QEMU.

See `.impossible/workflows/build.md` for the full workflow.
