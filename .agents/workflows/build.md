---
description: Build the OS from source and test in QEMU
---

# Build Workflow

// turbo-all

## Steps

1. Incremental build (default — only recompiles changed files):
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
bash scripts/build.sh run
```

## Build Modes

| Command | Description |
|---------|-------------|
| `bash scripts/build.sh` | Incremental build (fast — only changed files) |
| `bash scripts/build.sh clean` | Full clean build (removes build/ first) |
| `bash scripts/build.sh run` | Incremental build + launch QEMU |
| `bash scripts/build.sh clean run` | Full clean build + launch QEMU |

## Output

- Per-step progress banners with timing
- Error extraction on failure (highlights compiler errors)
- Summary footer with per-step durations
- Full log: `build/build.log`
- Sentinel: `tail -1 build/build.log` → `=== BUILD OK ===` or `=== BUILD FAILED ===`

## Troubleshooting

- **Triple fault / instant reboot:** Add `-d int,cpu_reset` to QEMU flags and check serial output
- **No output on screen:** Verify VGA buffer writes to `0xB8000` and check QEMU `-serial stdio` for kernel logs
- **Linker errors:** Check `linker.ld` for correct section layout and entry point
- **Build appears stuck:** Run `tail -1 build/build.log` — if it shows a sentinel, the build finished
