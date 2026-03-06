---
description: Build the OS from source and test in QEMU
---

# Build Workflow

// turbo-all

## Steps

1. Clean previous artifacts:
```bash
make clean
```

2. Build all targets (bootloader + kernel + ISO):
```bash
make all
```

3. Package the bootable ISO:
```bash
make iso
```

4. Launch QEMU to test the ISO:
```bash
make run
```

## Quick Rebuild (skip clean)

1. Rebuild only changed files:
```bash
make all
```

2. Test immediately:
```bash
make run
```

## Troubleshooting

- **Triple fault / instant reboot:** Add `-d int,cpu_reset` to QEMU flags and check serial output
- **No output on screen:** Verify VGA buffer writes to `0xB8000` and check QEMU `-serial stdio` for kernel logs
- **Linker errors:** Check `linker.ld` for correct section layout and entry point
- **ISO won't boot:** Verify GRUB config or MBR magic bytes (`0x55AA` at offset 510)
