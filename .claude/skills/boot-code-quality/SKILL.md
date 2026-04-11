---
name: boot-code-quality
description: Pre-flight quality checklist for UEFI bootloader code (src/boot/). Enforces UEFI error handling, memory ownership across ExitBootServices, ACPI/SMBIOS table safety, framebuffer guards, boot_info ABI sync, serial-before-klog rules, EBS boundary, POST16 coverage, and fallback chains. Auto-loads when editing src/boot/ files.
---

# Boot Code Quality

> This skill auto-loads when writing UEFI bootloader code. It is a pre-flight checklist, not a post-hoc review. Apply these rules WHILE writing code so it comes out correct the first time.

## When This Applies

Every time you create or modify a `.c`, `.h`, or `.asm` file under `src/boot/`. Does NOT apply to kernel code (`src/kernel/`, `include/kernel/`) -- use `kernel-code-quality` for those.

## Pre-Write Checklist

Before writing any boot code, walk through these gates. Each gate is pass/fail.

---

### Gate 1: UEFI Type Rules

- [ ] **UEFI types only.** Use `UINT8`, `UINT16`, `UINT32`, `UINT64`, `UINTN`, `BOOLEAN`, `CHAR16` -- never kernel types (`uint8_t`, `uint16_t`) in bootloader code.
- [ ] **UEFI calling convention.** All EFI function pointers use `EFIAPI` (`__attribute__((ms_abi))`). Missing this corrupts the stack on firmware calls.
- [ ] **No kernel headers.** Bootloader code includes `efi.h` and its own local struct copies of `boot_info`. Never include `kernel/types.h`, `kernel/klog.h`, etc.
- [ ] **ASCII only.** No Unicode dashes, no UTF-8 in strings or comments. Serial output goes to Windows terminals that garble multi-byte UTF-8.

### Gate 2: UEFI Error Handling

- [ ] **Every `gBS->*` call checked.** `gBS->LocateProtocol`, `gBS->AllocatePages`, `gBS->HandleProtocol` -- every one returns `EFI_STATUS`. Check with `EFI_ERROR(status)`. No silent failures.
- [ ] **Every `gST->*` access guarded.** `gST->ConOut` can be NULL on headless firmware. `gST->RuntimeServices` can be NULL. Check before dereference.
- [ ] **Error paths log to serial.** Use `serial_early_print("[FAIL] ...")` or `boot_fatal()` -- never silently return or HLT.
- [ ] **Status codes propagated.** If a sub-function fails, the caller must see the failure. No swallowing errors.

### Gate 3: Memory Ownership Across ExitBootServices

- [ ] **What survives EBS:** Runtime memory regions (EfiRuntimeServicesCode/Data), `boot_info` at `BOOT_INFO_PHYS_ADDR` (0x10000), page tables at 0x70000, framebuffer (if GOP provided one), and anything in `EfiLoaderCode`/`EfiLoaderData` (stays mapped but reclaimable by kernel PMM).
- [ ] **What dies at EBS:** All `gBS->*` function pointers, `gST->ConOut`, `gST->BootServices`, any `EfiBootServicesCode/Data` memory. Never store a pointer to boot-services memory in `boot_info`.
- [ ] **DMA buffers for hardware handover** (xHCI, NVMe) must be allocated in `EfiLoaderData` and tracked in `boot_info` so the kernel can reserve them from PMM.

### Gate 4: ACPI / SMBIOS Table Safety

- [ ] **Validate signature before parsing.** RSDP: check "RSD PTR " signature. SDT tables: check 4-byte signature. SMBIOS: check anchor string.
- [ ] **Bound all length fields.** `sdt->length` must be >= header size and <= sane max (1 MiB for SDTs). `hdr->length` in SMBIOS must fit within remaining table bytes.
- [ ] **Bound all string scans.** SMBIOS string extraction must stop at the table end, not scan until NUL. ACPI OEM strings are fixed-size, not NUL-terminated.
- [ ] **Validate before dereference.** Every table pointer from `gST->ConfigurationTable` or from an SDT entry list must be non-NULL and within a plausible physical address range before casting and reading.

### Gate 5: Framebuffer Safety

- [ ] **FrameBufferBase != 0 before any pixel write.** GOP can report FrameBufferBase=0 on headless firmware or after SetMode failure. Writing through address 0 crashes.
- [ ] **Pitch validation.** `PixelsPerScanLine >= HorizontalResolution`. If not, the mode is corrupt -- skip it.
- [ ] **Size bounds.** `height * pitch * 4` must not overflow `UINTN`. Reject modes where the framebuffer would exceed available memory.
- [ ] **Headless fallback.** If no usable framebuffer, set `fb_available = 0`, `hidpi = 0`, continue boot. Never fatal on missing display.

### Gate 6: boot_info ABI Sync

- [ ] **Struct layout match.** The bootloader's local `struct boot_info` (in `bootx64.c`) must exactly match `include/kernel/boot_info.h`. Same field order, same types, same padding. A mismatch means the kernel reads garbage from boot_info.
- [ ] **New fields go at the end** (before `secure_boot_enabled` / kernel-populated section). Never insert in the middle -- shifts all subsequent offsets.
- [ ] **Both structs updated together.** If you add a field to `boot_info.h`, add it to the bootloader's copy in the same commit.

### Gate 7: Serial Before klog

- [ ] **Bootloader uses `serial_early_print()` only.** Never call `klog()`, `printk()`, or `serial_write()` -- those are kernel functions.
- [ ] **Log format: `[BOOT] ...` prefix.** Consistent with kernel's `[subsystem]` format.
- [ ] **`boot_log_init()` before any `serial_early_print()`.** The boot log buffer captures serial output for ESP write; init it first.
- [ ] **Post-EBS serial is one-way.** After ExitBootServices, serial output still works (direct I/O port), but no UEFI console. `efi_print()` / `gST->ConOut` are dead.

### Gate 8: ExitBootServices Boundary

- [ ] **No `gBS->*` calls after ExitBootServices.** Boot Services are gone. Any call through `gBS` after EBS crashes.
- [ ] **No `gST->ConOut` after EBS attempted.** Even if EBS fails, ConOut may be in an undefined state. The `g_ebs_in_progress` flag gates this in `boot_fatal()`.
- [ ] **`gST->RuntimeServices` survives EBS** -- but only the function pointers, not the protocol handles. The kernel calls SVAM to remap.

### Gate 9: POST16 Coverage

- [ ] **Every boot phase step gets entry/exit POST codes.** POST16 is the ONLY diagnostic when serial isn't yet running (triple fault before `serial_early_init`).
- [ ] **POST16 codes are unique.** Check `boot_init.h` for collisions. The boot-time uniqueness scan catches duplicates.
- [ ] **Bootloader POST codes use 0xBxxx range.** Kernel Phase 0 uses 0x0xxx, Phase 1 uses 0x1xxx. Bootloader uses 0xBxxx to avoid collision.

### Gate 10: Fallback Chains

- [ ] **Every hardware probe has a graceful fallback.** GOP: mode 0 fallback, then headless. Serial: SPCR, then COM1, then COM2, then silent. USB: skip if no protocol. ACPI: warn and continue.
- [ ] **Never `for (;;) hlt;` without an error message.** Use `boot_fatal()` which shows an error screen + serial message + optional keypress + HLT.
- [ ] **No silent HLT.** If the bootloader cannot proceed, the user MUST see why. On screen if ConOut available, on serial always.
- [ ] **Explicit "not found" paths.** If a table/protocol/device is absent, log it explicitly: `"[BOOT] SPCR absent"`, `"[BOOT] GOP: headless mode"`. Silence is a bug.
