# Project Context — Impossible OS

> **Read this at the start of every session.** Update it after major changes, completed milestones, or discovered blockers.

## Current Status

| Area | Status | Notes |
|------|--------|-------|
| UEFI bootloader | ✅ Complete | GOP, ELF load, RSDP, boot_info, HiDPI, SVAM |
| Kernel core | 🔄 Active | PMM, VMM, scheduler, VFS, IPC working |
| UEFI runtime services | ✅ Complete | Pointer preservation across ExitBootServices() |
| AHCI + VirtIO storage | ✅ Complete | DMA paths working |
| Compositing desktop | ✅ Complete | TrueType fonts, blur, gradients |
| Networking | ✅ Complete | RTL8139, ARP, IPv4, ICMP, UDP, DHCP |
| Win32 API surface | 🔄 In progress | PE loader, syscall ABI, user-mode work ongoing |
| Registry | 🔄 In progress | Hive format, notifications, Win32 API |
| Exception dispatch / SEH | ⬜ Planned | TODO-10 in 02-kernel-core |

## Active Work

- Win32 PE loader and ABI (`02-kernel-core/`, `09-services-security/`)
- Shell loader regression investigation (cmd.exe loading in QEMU)
- AI agent infrastructure consolidation (this task)

## Recently Completed

- UEFI runtime services preservation (`src/kernel/uefi_runtime.c`, `src/boot/uefi/bootx64.c`)
- HiDPI auto-detection added to bootloader (width ≥ 2560 sets `boot_info.hidpi`)
- Secure Boot state written to registry during kernel init
- `.impossible/` agent-agnostic layer created

## Known Blockers / Issues

- Shell loader regression: `cmd.exe` not loading reliably in QEMU — see conversation `b771d393`
- NTFS B-tree bugs in directory insertion — see conversation `adc80ebb`

## Architecture Quick-Reference

```
UEFI Firmware → BOOTX64.EFI → kernel.exe (ELF) → Desktop Shell
                    │                │
              GOP + MemMap      boot_info struct
```

- **Build**: `bash scripts/build.sh` → check `tail -1 build/build.log`
- **Run**: `bash scripts/build.sh run` (QEMU)
- **Serial log**: `build/serial.log`
- **Debug crash**: `llvm-addr2line-19 -e build/kernel.exe -f <RIP>`

## TODO System

- Master index: `todo/TODO-00-INDEX.md`
- 14 domains, 86 TODO files, ~1500 sections
- Status: `[ ]` todo · `[/]` in-progress · `[x]` done
- Priority: 🔴 P0 · 🟠 P1 · 🟡 P2 · 🟢 P3

## Agent Notes

- **Cursor + Claude Code**: Source of truth. Use `.cursor/rules/` and `.cursor/skills/`
- **Antigravity**: Reads `AGENTS.md` + `.agents/workflows/` (redirect stubs to `.cursor/skills/`)
- **Copilot**: Reads `.github/copilot-instructions.md`
- **All agents**: Rules canonical content lives in `.impossible/rules/`
