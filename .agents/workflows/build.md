---
description: Build the OS from source and test in QEMU
---

# Build Workflow

// turbo-all

## Steps

1. Run the build wrapper (default: clean + all):
```bash
bash scripts/build.sh
```

2. Verify build succeeded:
```bash
tail -1 build/build.log
```
Expected output: `=== BUILD OK ===`

3. Launch QEMU to test:
```bash
make run
```

## Quick Rebuild (skip clean)

1. Rebuild only changed files:
```bash
bash scripts/build.sh all
```

2. Verify:
```bash
tail -1 build/build.log
```

3. Test:
```bash
make run
```

## Build Targets

| Target | Description |
|--------|-------------|
| `clean` | Remove `build/` directory |
| `all` | Compile bootloader + kernel |
| `run` | Launch QEMU |
| (default) | `clean all` |

Usage: `bash scripts/build.sh [targets...]`

Full build log: `build/build.log`

## Troubleshooting

- **Triple fault / instant reboot:** Add `-d int,cpu_reset` to QEMU flags and check serial output
- **No output on screen:** Verify VGA buffer writes to `0xB8000` and check QEMU `-serial stdio` for kernel logs
- **Linker errors:** Check `linker.ld` for correct section layout and entry point
- **Build appears stuck:** Run `tail -1 build/build.log` — if it shows a sentinel, the build finished
