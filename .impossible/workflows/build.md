---
description: Build the OS from source and test in QEMU
---

# Build

1. Run `bash scripts/build.sh` for an incremental build
2. Check `tail -1 build/build.log` — must show `=== BUILD OK ===`
3. Run `bash scripts/build.sh run` to launch QEMU
4. Monitor serial output in `build/serial.log`
