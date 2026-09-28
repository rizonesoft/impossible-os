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

### Gate 8: Kernel Serial Handoff Safety

> **Incident 2026-04-11:** A Codex quality review suggested changing `serial_init()` to unconditionally assign `s_serial_port = g_boot_info.serial_port` instead of keeping the COM1 default when 0. This killed ALL kernel serial output because early klog runs before `serial_init()` using the static default `0x3F8`. The fix was blindly applied without verifying the pre-serial_init window. Lesson: boot_info fields may not be populated yet when early kernel code runs.

- [ ] **Kernel `s_serial_port` default (0x3F8) must survive until `serial_init()`.** Early klog writes to serial BEFORE `serial_init()` runs. The static default `0x3F8` is the only working port during this window. `serial_init()` reads `g_boot_info.serial_port` and overrides -- but only if non-zero.
- [ ] **Never set `s_serial_port = 0` from boot_info.** If bootloader reports `serial_port = 0` (no UART found), the kernel keeps the COM1 default for the pre-init window. `serial_init()` returns early if port is 0, making subsequent serial calls no-ops -- but early output still works.
- [ ] **Test after any serial.c change:** does early kernel boot still produce serial output? If `[PHASE0] SERIAL` doesn't appear, you broke the handoff.

### Gate 9: ExitBootServices Boundary

- [ ] **No `gBS->*` calls after ExitBootServices.** Boot Services are gone. Any call through `gBS` after EBS crashes.
- [ ] **No `gST->ConOut` after EBS attempted.** Even if EBS fails, ConOut may be in an undefined state. The `g_ebs_in_progress` flag gates this in `boot_fatal()`.
- [ ] **`gST->RuntimeServices` survives EBS** -- but only the function pointers, not the protocol handles. The kernel calls SVAM to remap.

### Gate 10: POST16 Coverage

- [ ] **Every boot phase step gets entry/exit POST codes.** POST16 is the ONLY diagnostic when serial isn't yet running (triple fault before `serial_early_init`).
- [ ] **POST16 codes are unique.** Check `boot_init.h` for collisions. The boot-time uniqueness scan catches duplicates.
- [ ] **Bootloader POST codes use 0xBxxx range.** Kernel Phase 0 uses 0x0xxx, Phase 1 uses 0x1xxx. Bootloader uses 0xBxxx to avoid collision.

### Gate 11: QR Code Correctness

> **Incident 2026-04-11:** The QR encoder had 6 bugs across two iterations, each making it unscannable. Codex caught 1 of 6 (RS coefficients). Codex was actively wrong about 1 (said format bits should be LSB-first). The remaining 4 were found via real-hardware testing + segno comparison. Lesson: QR encoding has many interacting spec details that AI reviewers cannot reliably verify. Always diff against a reference library.

- [ ] **Verify against segno.** Any change to the QR encoder MUST produce a matrix with 0 differences against `segno.make(url, version=3, error='L', mask=0, boost_error=False)`. Phone-scan the output. This is non-negotiable -- the spec has too many subtleties for manual verification.
- [ ] **Pixel colors: 0x00000000 = black, 0x00FFFFFF = white.** GOP framebuffers use BGRX format. `0xFF000000` is blue, not black.
- [ ] **Format info placement uses segno's algorithm.** Vertical col 8 gets bits LSB-first (bit 0 at row 0), horizontal row 8 gets bits MSB-first (bit 14 at col 0). Copy 2: row 8 right edge gets LSB-first, col 8 bottom edge gets MSB-first. Do NOT change this without verifying against segno.
- [ ] **Zigzag column loop: use separate iterator.** `for (col_iter = SIZE-1; col_iter >= 1; col_iter -= 2) { right = col_iter; if (right <= 6) right--; }` -- modifying `right` inside the loop must NOT affect the iteration counter. A Python `for range()` loop ignores modifications to the loop variable; C does not.
- [ ] **Zigzag direction: `(right & 2) == 0`, then XOR with `(col < 6)`.** Columns left of the timing column (col 6) flip direction. This is per ISO 18004 section 7.7.3 and segno's implementation.
- [ ] **Byte padding: always pad to next codeword boundary.** After terminator, add `8 - (nbits % 8)` zero bits even when already byte-aligned (adds a full 0x00 codeword). This matches segno's behavior and produces correct padding before the 0xEC/0x11 alternation.
- [ ] **RS coefficients for V3 ECL-L (15 EC codewords): `{0x1D, 0xC4, 0x6F, 0xA3, 0x70, 0x4A, 0x0A, 0x69, 0x69, 0x8B, 0x84, 0x97, 0x20, 0x86, 0x1A}`.** Verify via independent computation if changed.

### Gate 12: Fallback Chains

- [ ] **Every hardware probe has a graceful fallback.** GOP: mode 0 fallback, then headless. Serial: SPCR, then COM1, then COM2, then silent. USB: skip if no protocol. ACPI: warn and continue.
- [ ] **Never `for (;;) hlt;` without an error message.** Use `boot_fatal()` which shows an error screen + serial message + optional keypress + HLT.
- [ ] **No silent HLT.** If the bootloader cannot proceed, the user MUST see why. On screen if ConOut available, on serial always.
- [ ] **Explicit "not found" paths.** If a table/protocol/device is absent, log it explicitly: `"[BOOT] SPCR absent"`, `"[BOOT] GOP: headless mode"`. Silence is a bug.

### Gate 13: Spec Compliance -- No Sub-Standard Code

> **Incident 2026-04-12:** Device path walk used `Type == 0x7F` instead of `Type == 0x7F && SubType == 0xFF` for END_ENTIRE. Technically worked but violated UEFI spec Table 10-1. Claude accepted this as "forward-reserve" instead of fixing it immediately. The user had to ask.

- [ ] **Every UEFI protocol/structure usage matches the spec.** If the UEFI spec defines a field, constant, or subtype, use it. Do not take shortcuts that happen to work on tested firmware but diverge from the spec. Examples: device path END nodes have subtypes (0xFF=Entire, 0x01=Instance); HardDrive DP has both MBRType AND SignatureType; GetVariable returns EFI_BUFFER_TOO_SMALL not a truncated prefix.
- [ ] **No "works on QEMU" shortcuts.** QEMU/OVMF is lenient. Real firmware (AMI, Phoenix, Insyde) is not. If the spec says check a field, check it -- even if QEMU doesn't care.
- [ ] **Fix immediately, don't defer.** If you notice sub-standard code during implementation or review, fix it in the same commit. Do not accept it as "forward-reserve" or "future enhancement." Sub-standard code that works today breaks on the next firmware update.
- [ ] **Validate all fields the spec defines for a structure.** If you're parsing a UEFI table node and only checking 2 of 4 defined fields, you're cutting corners. Check all fields that affect correctness.

### Gate 14: Parse Buffers -- Dynamic First, Hard-Fail on Overflow

> **Incident 2026-04-21:** Editing `resources/boot/boot.conf` grew the file past the bootloader's hardcoded 4096-byte read buffer. The bootloader WARN-and-truncated at 4096 bytes, silently dropping the patch-appended `test=1` line at EOF. Every `test=1` boot then saw `test=0` and ran zero tests. A `[WARN]` line plus a normal-looking boot is too easy to miss.

- [ ] **Prefer dynamic allocation over fixed-size parse buffers.** For disk-sourced input (config files, cmdline, cert blobs), query the file size via `GetInfo(&EFI_FILE_INFO_ID, ...)` then `gBS->AllocatePool` exactly that size. `boot_conf_load()` in `bootx64.c` is the reference implementation: size-probe -> AllocatePool for `EFI_FILE_INFO` -> read FileSize -> AllocatePool(FileSize+1) -> Read -> parse -> FreePool. No arbitrary cap that has to be raised every time a file grows.
- [ ] **Sanity cap only, and hard-fail on hit.** A dynamic allocator still needs a ceiling so a corrupt/malicious filesystem cannot request a multi-GiB allocation. `BOOT_CONF_SANITY_CAP = 1 MiB` in `bootx64.c` is the shape: any legitimate boot.conf is under 100 KiB, so the 1 MiB cap only fires when something is genuinely wrong -- and then `boot_fatal(BOOT_ERR_CONF_INVALID, ...)`, never `[WARN]` + continue. Silent truncation of the tail is how the 2026-04-21 incident hid.
- [ ] **If a fixed-size buffer is unavoidable, document the cap next to it + hard-fail on overflow.** Some inputs really are bounded (e.g. a 64-char product code). Put the cap in a `#define`, note the hard upper bound in a comment, and `boot_fatal()` on overflow with the actual byte count in the error. `[WARN] truncating` + continuing is never acceptable.
- [ ] **Check the consumer's handling before editing a tracked resource file.** When adding content to anything parsed at boot (e.g. `resources/boot/boot.conf`, future `resources/boot/cmdline.txt`), verify the consumer either (a) allocates dynamically or (b) has headroom for your addition. `grep` for the buffer declaration in the consumer; if the file has grown dramatically, proactively switch the consumer to dynamic-allocation before the next edit breaks it.

### Gate 15: Uninitialised Loader `.bss` -- Reset First, Wide Volatile Cookie

> **Incidents 2026-09 (TODO-14 sections 18 and 20):** UEFI does not zero BOOTX64's `.bss`, so a static read before its initialiser holds firmware poison (`0xAF` bytes) or the previous boot's value, never 0. Section 18 drove port `0xAFAF` as a UART; section 20 found `boot_entries_parser.c` skipping its CRC table build and rejecting a VALID boot store as `CRC_MISMATCH`. Neither reproduces under any emulator. Full history: `docs/infrastructure/bare-metal-gotchas.md` "Uninitialised `.bss` in the Loader".

- [ ] **Reset loader statics as the first statement of `efi_main`.** Any static a helper may read before its own initialiser runs is reset there, before the first call that could consult it.
- [ ] **Readiness is a wide exact-match cookie tested with `!=`, never a boolean.** `if (!ready)` is wrong: `0xAF` is non-zero and reads as "configured". Use a 32-bit magic (`EARLY_DIAG_READY` in `src/boot/uefi/bootx64.c:424`, `BOOT_ENTRIES_CRC32_READY` in `src/boot/uefi/boot_entries_parser.c:61`) and compare `!= MAGIC`.
- [ ] **Mark the cookie `volatile`.** Without it clang narrows a write-once flag to one byte and deletes the wide compare: measured on the real `-O2` objects, `static int g_crc32_ready` was emitted at SIZE 1.
- [ ] **Early serial gates on the cookie, not on port truthiness (extends Gate 7).** `serial_early_print` / `serial_early_putchar` test `s_serial_ready != EARLY_DIAG_READY`; a non-zero `s_serial_port` proves nothing, because poison is non-zero.
