# P0002 — Bootloader & Secure Boot

> **Goal:** Build a custom UEFI bootloader, implement a Windows 11-style boot splash,
> and harden for real-world hardware with Secure Boot via the shim chain-loading approach.
> Sign our bootloader with a Machine Owner Key (MOK) and submit our shim to Microsoft
> for signing. Improve boot UX with resolution auto-detection, fade-in transitions,
> and boot profiling.

> [!IMPORTANT]
> **Secure Boot Strategy:** We opt in to get our own shim signed by Microsoft via the
> [rhboot/shim-review](https://github.com/rhboot/shim-review) process. This eliminates
> the MOK enrollment popup for end users. Until then, users can either disable Secure Boot
> or use the MOK enrollment flow.

> [!CAUTION]
> **Private Key Security:** The MOK private key (`MOK.key`) MUST NEVER be committed to
> any repository. Store it in a secure hardware token or encrypted vault. If compromised,
> an attacker could sign malware that bypasses Secure Boot on enrolled machines.

---

## 1. Custom UEFI Bootloader ✅

### 1.1 UEFI Boot Application

**Prompt:** This section is marked complete. Verify the implementation is correct: review `src/boot/uefi/bootx64.c` to confirm it implements a PE/COFF binary at `\EFI\BOOT\BOOTX64.EFI` that replaces GRUB. Confirm it uses the `ms_abi` calling convention, initializes the UEFI system table and boot services, disables the watchdog timer, and passes control to the kernel. Check that the Makefile builds `BOOTX64.EFI` as a PE binary and copies it to the EFI staging directory. Confirm the commit `"boot: custom UEFI bootloader replaces GRUB"` exists in git history. Run `bash scripts/build.sh clean` and verify the ISO boots in QEMU. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to the UEFI bootloader or GRUB. Add notes, gotchas, and design decisions directly in this TODO section covering the custom UEFI boot application architecture.

- [x] Create `src/boot/uefi/bootx64.c` — UEFI boot application entry point (`efi_main`)
- [x] Implement `ms_abi` calling convention for UEFI compatibility
- [x] Initialize `EFI_SYSTEM_TABLE`, `EFI_BOOT_SERVICES`, `EFI_HANDLE`
- [x] Disable UEFI watchdog timer (default 5-minute timeout)
- [x] Create `src/boot/uefi/efi.h` — UEFI type definitions and protocol GUIDs
- [x] Build as PE/COFF binary (`BOOTX64.EFI`) via Makefile
- [x] Commit: `"boot: custom UEFI bootloader replaces GRUB"` (`32a5f8f`)

### 1.2 GOP Framebuffer Initialization

**Prompt:** This section is marked complete. Verify that `init_gop()` in `bootx64.c` locates the `EFI_GRAPHICS_OUTPUT_PROTOCOL`, sets the video mode to 1280×720×32bpp with `PixelBlueGreenRedReserved8BitPerColor` format, and stores the framebuffer address, width, height, and pitch in the boot info struct. Confirm `fill_screen_black()` clears the framebuffer immediately after GOP init for a clean transition. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to GOP or framebuffer initialization. Add notes, gotchas, and design decisions directly in this TODO section covering GOP mode selection and framebuffer setup.

- [x] Implement `init_gop()` — locate `EFI_GRAPHICS_OUTPUT_PROTOCOL` via `LocateProtocol`
- [x] Set video mode to 1280×720×32bpp (`PixelBlueGreenRedReserved8BitPerColor`)
- [x] Store framebuffer address, width, height, pitch in `boot_info.fb`
- [x] Implement `fill_screen_black()` — clear screen after GOP init
- [x] Commit: `"boot: fix UEFI bootloader — ms_abi calling convention + PE reloc + kernel_main lookup"` (`d11f61f`)

### 1.3 Kernel ELF Loader

**Prompt:** This section is marked complete. Verify that `load_kernel()` in `bootx64.c` reads `\boot\kernel.exe` from the EFI partition using `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`, parses the ELF64 header, loads all `PT_LOAD` program headers into memory at their specified physical addresses, and returns the ELF entry point address. Confirm it handles both LOAD segments (code + data) and zeroes BSS. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to kernel loading. Add notes, gotchas, and design decisions directly in this TODO section covering the ELF64 kernel loading process.

- [x] Open `\boot\kernel.exe` via `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`
- [x] Parse ELF64 header (verify magic, class, machine `EM_X86_64`)
- [x] Load `PT_LOAD` segments into memory at specified physical addresses
- [x] Zero BSS regions (file size < memory size)
- [x] Return kernel entry point from ELF header
- [x] Commit: `"boot: custom UEFI bootloader replaces GRUB"` (`32a5f8f`)

### 1.4 ACPI RSDP Discovery

**Prompt:** This section is marked complete. Verify that `find_acpi_rsdp()` in `bootx64.c` searches the UEFI configuration table for the ACPI 2.0 GUID (`EFI_ACPI_20_TABLE_GUID`) and falls back to ACPI 1.0 (`EFI_ACPI_TABLE_GUID`). Confirm the RSDP address and ACPI version are stored in boot info for kernel ACPI parsing. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to ACPI discovery. Add notes, gotchas, and design decisions directly in this TODO section covering ACPI RSDP discovery and boot info handoff.

- [x] Search UEFI config tables for `ACPI_20_TABLE_GUID` (prefer v2.0)
- [x] Fall back to `ACPI_TABLE_GUID` (v1.0)
- [x] Store RSDP address and version in `boot_info.acpi_rsdp_addr` / `acpi_version`
- [x] Commit: `"boot: custom UEFI bootloader replaces GRUB"` (`32a5f8f`)

### 1.5 Memory Map & Page Tables

**Prompt:** This section is marked complete. Verify that `get_memory_map()` retrieves the UEFI memory map, `fill_memory_map()` converts UEFI memory types to Multiboot2-compatible types, and `setup_page_tables()` creates identity-mapped page tables covering 4 GiB. Confirm `ExitBootServices()` is called correctly (with retry on stale map key). After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to memory mapping or page tables. Add notes, gotchas, and design decisions directly in this TODO section covering the UEFI memory map, page table setup, and ExitBootServices flow.

- [x] Implement `get_memory_map()` — call UEFI `GetMemoryMap` with retry on buffer resize
- [x] Implement `fill_memory_map()` — convert UEFI memory descriptors to boot info entries
- [x] Map UEFI memory types to Multiboot2 types (available, reserved, ACPI, NVS, bad)
- [x] Call `ExitBootServices()` with the map key (retry once on `EFI_INVALID_PARAMETER`)
- [x] Implement `setup_page_tables()` — 4-level identity mapping of 4 GiB via 2 MiB pages
- [x] Implement `jump_to_kernel()` — set up Multiboot2 magic + boot info pointer, jump
- [x] Commit: `"boot: custom UEFI bootloader replaces GRUB"` (`32a5f8f`)

---

## 2. Boot Splash Screen ✅

### 2.1 Windows 11-Style Boot Splash

**Prompt:** This section is marked complete. Verify that `src/kernel/boot_splash.c` implements a persistent boot splash with: black background, centered icon at ~40% vertical, animated horizontal dots below the icon, and status text below dots. Confirm `boot_splash_init()` locks the compositor to prevent printk output on screen, `boot_splash_finish()` unlocks it and clears the screen for the desktop. Verify the splash is active from `fb_init()` through desktop startup. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to the boot splash. Add notes, gotchas, and design decisions directly in this TODO section covering the boot splash architecture and lifecycle.

- [x] Create `src/kernel/boot_splash.c` and `include/kernel/boot_splash.h`
- [x] Implement `boot_splash_init()` — compute layout, lock compositor, draw initial splash
- [x] Implement `boot_splash_finish()` — stop animation, clear screen, unlock compositor
- [x] Implement `boot_splash_active()` — return whether splash is currently showing
- [x] Black background with centered icon at ~40% vertical position
- [x] Status text area below dots — "Starting...", "Detecting hardware...", etc.
- [x] Lock compositor during splash (`fb_lock_compositor()`) — serial-only klog output
- [x] Suppress klog framebuffer output during splash (`klog_set_screen_level(LOG_FATAL)`)
- [x] Restore klog screen level to `LOG_INFO` when splash finishes
- [x] Remove UEFI "Loading kernel..." text for clean black→splash transition
- [x] Commit: `"boot: persistent Windows 11-style boot splash"` (`4d61756`)

### 2.2 Animated Dot Wave

**Prompt:** This section is marked complete. Verify the dot animation in `boot_splash.c` uses PIT timer callbacks at ~14 fps. Confirm the wave-style animation with 6 dots, pulsing radius (min 5px → max 8px), staggered phases, and smooth sine-like easing. Verify `boot_splash_start_animation()` is called after `pit_init() + sti`, and `pit_unregister_callback()` is called on finish. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to the dot animation. Add notes, gotchas, and design decisions directly in this TODO section covering the PIT-driven animation system and wave parameters.

- [x] Implement PIT-driven animation callback at ~14 fps
- [x] Implement wave-style dot animation (6 dots, pulsing radius)
- [x] Dot radius range: 5px min → 8px max
- [x] Dot spacing: 20px center-to-center
- [x] Wave stagger: 2 frames between each dot's phase start
- [x] Seamless wave wrap (no rest gap between cycles)
- [x] Smooth sine-like easing using integer approximation table
- [x] Implement `boot_splash_start_animation()` — register PIT callback
- [x] Commit: `"boot: 6 dots (sweet spot between 5 and 8)"` (`d23af93`)

### 2.3 Boot Icon Embedding

**Prompt:** This section is marked complete. Verify that `tools/convert_icon.py` reads a PNG from `resources/boot/` and generates `src/kernel/boot_splash_icon.h` containing raw BGRA pixel data as a C array. Confirm the Makefile has a `boot-icon` target with the correct dependency. Verify the current icon is `boot_96.png` (96×96). After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to boot icon embedding. Add notes, gotchas, and design decisions directly in this TODO section covering the PNG-to-C-header icon pipeline and supported resolutions.

- [x] Create `tools/convert_icon.py` — PNG to C header converter (BGRA pixel data)
- [x] Implement minimal PNG decoder (IHDR, IDAT, deflate, unfilter)
- [x] Generate `boot_splash_icon.h` with `BOOT_ICON_W`, `BOOT_ICON_H`, `boot_icon_data[]`
- [x] Add Makefile `boot-icon` target: `resources/boot/boot_96.png → boot_splash_icon.h`
- [x] Support alpha-blended icon rendering (BGRA → framebuffer with alpha compositing)
- [x] Multi-resolution source PNGs: `boot_96.png`, `boot_128.png`, `boot_192.png`, `boot_256.png`, `boot_288.png`
- [x] Commit: `"boot: new boot logo (boot_96.png) + updated start menu icon"` (`2adee94`)

### 2.4 Anti-Aliased TTF Font for Status Text

**Prompt:** This section is marked complete. Verify that `tools/convert_boot_font.py` embeds Selawik Semibold (`selawksb.ttf`) as a C header with pre-rasterized glyphs. Confirm the boot splash uses `boot_font_init(16)` for 16px font, `boot_font_measure()` for text width, and `boot_font_render()` for anti-aliased rendering. Verify 1px letter spacing is applied. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to boot fonts. Add notes, gotchas, and design decisions directly in this TODO section covering the TTF-to-C-header font pipeline, the boot font API, and anti-aliased rendering.

- [x] Create `tools/convert_boot_font.py` — TTF to C header (pre-rasterized glyphs)
- [x] Embed Selawik Semibold font at 16px size
- [x] Implement `boot_font_init(pixel_size)` — init embedded font data
- [x] Implement `boot_font_measure(text)` — return text width in pixels
- [x] Implement `boot_font_render(text, x, y, color)` — render with anti-aliasing
- [x] Add 1px letter spacing (`BOOT_LETTER_SPACING`)
- [x] Makefile `boot-font` target: `resources/fonts/selawksb.ttf → boot_splash_font_data.h`
- [x] Commit: `"boot: anti-aliased TTF font for splash status text"` (`dbca624`)

### 2.5 Granular Boot Status Messages

**Prompt:** This section is marked complete. Verify that `main.c` calls `boot_splash_status()` with descriptive messages throughout the boot sequence: "Setting up hardware...", "Detecting hardware...", "Detecting drives...", "Configuring network...", "Loading system configuration...", "Preparing desktop...", "Loading fonts...", "Loading resources...", "Almost ready...". Each message should be followed by `boot_splash_tick()`. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to boot status messages. Add notes, gotchas, and design decisions directly in this TODO section covering the boot sequence stages and splash status message flow.

- [x] "Setting up hardware..." — GDT, IDT, PIT, heap, PMM
- [x] "Detecting hardware..." — PCI scan, NIC, input devices
- [x] "Detecting drives..." — partition scan, filesystem mount
- [x] "Configuring network..." — DHCP discover
- [x] "Loading system configuration..." — Registry, kernel tests
- [x] "Preparing desktop..." — log flush, desktop setup
- [x] "Loading fonts..." — TTF font manager init
- [x] "Loading resources..." — icon store, cursors
- [x] "Almost ready..." — window manager init
- [x] Commit: `"boot: add more granular splash status messages"` (`3e40e0f`)

---

## 3. Secure Boot — Shim Chain-Loading

### 3.1 Generate MOK Key Pair

**Prompt:** Verified (2026-03-14). RSA-2048 MOK key pair generated (`518993a`). Keys stored in `keys/`: `MOK.key` (gitignored), `MOK.cer` and `MOK.der` committed. Valid 2026-03-14 → 2036-03-11. SHA-256 fingerprint: `D3:6B:BA:F0:FD:56:D8:5D:B9:F6:9E:3F:29:73:C4:51:47:7A:C3:B3:40:A4:AD:13:7E:8E:67:A1:69:BC:03:D7`. `keys/README.md` documents key purpose, storage, and rotation. Check `.gitignore` contains `keys/MOK.key`.

- [x] Create `keys/` directory (add to `.gitignore`)
- [x] Generate MOK key pair (RSA-2048, `-days 3650`, CN=`Impossible OS Secure Boot Key`)
- [x] Convert to DER format for UEFI: `openssl x509 -in keys/MOK.cer -out keys/MOK.der -outform DER`
- [x] Create `keys/README.md` documenting key purpose and rotation
- [x] Add `keys/MOK.key` to `.gitignore` (NEVER commit private keys)
- [x] Commit: `"boot: MOK key generation infrastructure"` (`518993a`)

### 3.2 Sign Bootloader with MOK

**Prompt:** Verified (2026-03-14). `sign-efi` Makefile target added. `sbsigntool` was already installed (v0.9.4). Signing runs after `uefi-boot`, before `system-disk`. If `keys/MOK.key` is absent, signing is skipped with a clear message (dev builds unaffected). `build.sh` shows EFI Signing as step [5/6]. Verified: `[SIGN] Signature verified OK` in `build/build.log`. Commit: `"boot: sign EFI bootloader with MOK"`. PE/COFF gap warnings from sbsign are cosmetic and do not affect the signature. Run `bash scripts/build.sh clean` and grep for `[SIGN] Signature verified OK` to confirm.

- [x] Install `sbsigntool` (already installed: v0.9.4)
- [x] Add Makefile `sign-efi` target: signs `build/tools/BOOTX64.EFI` with `keys/MOK.key`
- [x] Verify signature: `sbverify --cert keys/MOK.cer build/tools/BOOTX64.EFI` → `[SIGN] Signature verified OK`
- [x] Update `scripts/build.sh` to call `sign-efi` as step [5/6] between EFI Boot and System Disk
- [x] If `keys/MOK.key` is missing, skip signing with clear message (dev builds unaffected)
- [x] Commit: `"boot: sign EFI bootloader with MOK"`


### 3.3 Build and Package Shim

**Prompt:** Verified (2026-03-14). `shim/shimx64.efi` and `shim/mmx64.efi` are built from rhboot/shim v16.1 with `VENDOR_CERT_FILE=keys/MOK.cer` embedded. The Makefile `system-disk` target automatically uses the shim chain-load layout when `shim/shimx64.efi` is present: `BOOTX64.EFI` ← shimx64.efi, `grubx64.efi` ← our signed bootloader, `mmx64.efi` ← MokManager. Rebuild script at `scripts/build-shim.sh`. SHA256: `d7e21770...` (shimx64.efi), `0141578f...` (mmx64.efi). Committed as `"boot: shim packaging with vendor certificate"`. Verify: run `bash scripts/build.sh clean`, grep `build/build.log` for `[DISK] Shim found — using Secure Boot chain-load layout`, and confirm `tail -1 build/build.log` shows `=== BUILD OK ===`. Confirm `docs/architecture/secure-boot.md` and updated `docs/architecture/bootloader.md` exist. Confirm `README.md` lists Secure Boot as a feature.

- [x] Fork `rhboot/shim` into public `impossible-os-shim` repo
- [x] Embed our `MOK.cer` as vendor certificate (in `Makefile`: `VENDOR_CERT_FILE`)
- [x] Build shim: `make VENDOR_CERT_FILE=MOK.cer ARCH=x86_64 shimx64.efi mmx64.efi`
- [x] Package EFI partition:
  ```
  \EFI\BOOT\
    ├── BOOTX64.EFI      ← shimx64.efi (renamed — firmware loads this)
    ├── grubx64.efi       ← Our bootloader (signed with MOK.key)
    └── mmx64.efi         ← MokManager (for first-boot key enrollment)
  ```
- [x] Test in QEMU with Secure Boot enabled (OVMF + enrolled PK/KEK)
- [x] Commit: `"boot: shim packaging with vendor certificate"`

### 3.4 Submit Shim for Microsoft Signing

**Prompt:** ✅ VERIFICATION — The shim-review submission has been filed. Verify the
following are still correct: (1) `rizonesoft/shim-review` branch
`rizonesoft-shim-x86_64-20260314` contains `shimx64.efi`, `MOK.cer`, `Dockerfile`,
`build.log`, and `README.md`; (2) SHA256 of `shim/shimx64.efi` matches
`d7e21770b1c8f2b977db1d533f7bba3d0de3d212e83ffd35c2509de970d6bd2f`; (3) the
shim-review issue is open on `rhboot/shim-review`; (4) `docs/architecture/shim-review.md`
exists and is accurate. Run `bash scripts/build.sh clean` and verify `=== BUILD OK ===`.
Fix any inconsistencies found. When Microsoft returns the signed binary, replace
`shim/shimx64.efi` and rebuild.

> [!IMPORTANT]
> **The README is NOT what reviewers evaluate.** The shim-review GitHub Issue is what
> matters. Fill out every field in the issue template completely — incomplete submissions
> are deprioritised. Respond quickly to reviewer questions; active projects are prioritised.

**What reviewers actually check:**

| Requirement | What they look for |
|-------------|-------------------|
| Reproducible build | Anyone can clone the shim fork and produce a byte-identical binary |
| SHA256 match | Hash in the issue matches the submitted binary exactly |
| Unmodified shim | No patches unless clearly documented and justified |
| Vendor certificate | `MOK.cer` correctly set as `VENDOR_CERT_FILE` in shim build |
| OS description | Legitimate use case — not malware, not for bypassing restrictions |
| Contact info | Real person responsible for the key |
| Public shim fork | Source of the exact build is publicly accessible |

**Fast-approval checklist:**
- [x] Fill out every field in the shim-review issue template — leave nothing blank
- [x] Use a Docker-based reproducible build (easiest way to prove identical output)
- [x] Verify `VENDOR_CERT_FILE=MOK.cer` is set correctly in shim Makefile
- [ ] Respond to reviewer questions within 24 hours *(pending — awaiting reviewers)*
- [x] Clearly state this is a legitimate OS project, not a tool to bypass restrictions

**UEFI CA Key context (important for timing):**

| Key | Expires | Status |
|-----|---------|--------|
| Microsoft UEFI CA 2011 | ~June 2026 | ⚠️ Expiring — new shims will NOT use this |
| Microsoft UEFI CA 2023 | ~2075 | ✅ New shims are signed with this |

Applying now means our shim will be signed with the **2023 CA** — valid for decades.
Older firmware (pre-2022) may need a BIOS update to trust the 2023 CA; disabling
Secure Boot is the simplest workaround for those users in the interim.

**Submission process:**
- [x] Open an issue on `rhboot/shim-review` with:
  - [x] Link to our public shim fork (`rizonesoft/shim-review` branch `rizonesoft-shim-x86_64-20260314`)
  - [x] SHA256 hash of the built `shimx64.efi` (`d7e21770b1c8f2b977db1d533f7bba3d0de3d212e83ffd35c2509de970d6bd2f`)
  - [x] Explanation of what Impossible OS is and why we need signing
  - [x] Reproducible build instructions (Docker — `shim-review/Dockerfile`)
  - [x] Contact name and email for the key holder (`derick@rizonetech.com`)
- [ ] Respond to Microsoft reviewer feedback *(pending — typically 2–4 weeks)*
- [ ] Receive signed `shimx64.efi` binary *(pending)*
- [ ] Replace unsigned shim with Microsoft-signed binary in ISO build *(pending)*
- [ ] Commit: `"boot: Microsoft-signed shim submitted"` *(after binary received)*

> [!IMPORTANT]
> **The README is NOT what reviewers evaluate.** The shim-review GitHub Issue is what
> matters. Fill out every field in the issue template completely — incomplete submissions
> are deprioritised. Respond quickly to reviewer questions; active projects are prioritised.

**What reviewers actually check:**

| Requirement | What they look for |
|-------------|-------------------|
| Reproducible build | Anyone can clone the shim fork and produce a byte-identical binary |
| SHA256 match | Hash in the issue matches the submitted binary exactly |
| Unmodified shim | No patches unless clearly documented and justified |
| Vendor certificate | `MOK.cer` correctly set as `VENDOR_CERT_FILE` in shim build |
| OS description | Legitimate use case — not malware, not for bypassing restrictions |
| Contact info | Real person responsible for the key |
| Public shim fork | Source of the exact build is publicly accessible |

**Fast-approval checklist:**
- [ ] Fill out every field in the shim-review issue template — leave nothing blank
- [ ] Use a Docker-based reproducible build (easiest way to prove identical output)
- [ ] Verify `VENDOR_CERT_FILE=MOK.cer` is set correctly in shim Makefile
- [ ] Respond to reviewer questions within 24 hours
- [ ] Clearly state this is a legitimate OS project, not a tool to bypass restrictions

**UEFI CA Key context (important for timing):**

| Key | Expires | Status |
|-----|---------|--------|
| Microsoft UEFI CA 2011 | ~June 2026 | ⚠️ Expiring — new shims will NOT use this |
| Microsoft UEFI CA 2023 | ~2075 | ✅ New shims are signed with this |

Applying now means our shim will be signed with the **2023 CA** — valid for decades.
Older firmware (pre-2022) may need a BIOS update to trust the 2023 CA; disabling
Secure Boot is the simplest workaround for those users in the interim.

**Submission process:**
- [ ] Open an issue on `rhboot/shim-review` with:
  - [ ] Link to our public shim fork
  - [ ] SHA256 hash of the built `shimx64.efi`
  - [ ] Explanation of what Impossible OS is and why we need signing
  - [ ] Reproducible build instructions (Docker preferred)
  - [ ] Contact name and email for the key holder
- [ ] Respond to Microsoft reviewer feedback (typically 2–4 weeks)
- [ ] Receive signed `shimx64.efi` binary
- [ ] Replace unsigned shim with Microsoft-signed binary in ISO build
- [ ] Commit: `"boot: Microsoft-signed shim submitted"`

**What must be public vs private:**

| Component                           | Must be public? | Reason                               |
|-------------------------------------|-----------------|--------------------------------------|
| Shim fork (tiny first-stage loader) | ✅ Yes           | Microsoft reviews this               |
| `bootx64.c` (our bootloader)        | ❌ No            | Shim just checks our MOK signature   |
| Kernel source                       | ❌ No            | UEFI/shim never sees the kernel      |
| Impossible OS codebase              | ❌ No            | Completely irrelevant to Secure Boot |

**User experience by audience:**

| Audience | Secure Boot approach |
|----------|---------------------|
| Developers / enthusiasts | Disable Secure Boot — simplest, zero friction |
| Tech-savvy, SB enabled | Enroll MOK once — works forever after |
| General users (target) | Microsoft-signed shim — zero friction once approved |
| Enterprise / kiosk | Microsoft-signed shim required — pending approval |

### 3.5 Interim: Ship Pre-Signed Shim (Before Microsoft Signing)

**Prompt:** While waiting for Microsoft to sign our shim, use Ubuntu's or Fedora's already-signed `shimx64.efi` as an interim solution. This binary is BSD-licensed and freely redistributable. The tradeoff: users will see a one-time MOK enrollment prompt on first boot (because the distro's shim has the distro's key baked in, not ours). This is the same approach used by Arch Linux. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, and commit as `"boot: interim pre-signed shim with MOK enrollment"`. Update `README.md` if it contains stale or incorrect references to Secure Boot or MOK enrollment. Add notes, gotchas, and design decisions directly in this TODO section covering the interim shim approach, MOK enrollment user flow, and install guide instructions.

- [ ] Download Ubuntu's signed `shimx64.efi.signed` from the `shim-signed` package
- [ ] Download Ubuntu's `mmx64.efi.signed` (MokManager)
- [ ] Package in ISO with correct EFI layout
- [ ] Test first-boot MOK enrollment flow:
  1. Shim loads → checks `grubx64.efi` → "Not signed by Canonical" → ❌
  2. Shim falls back to MokManager → asks user to enroll our MOK
  3. User confirms (requires physical presence — press keys + enter password)
  4. MOK stored in NVRAM → all future boots work automatically ✅
- [ ] Document MOK enrollment in install guide
- [ ] Commit: `"boot: interim pre-signed shim with MOK enrollment"`

**First-boot user experience by audience:**

| Audience                 | MOK enrollment acceptable?                        |
|--------------------------|---------------------------------------------------|
| Developers / enthusiasts | ✅ Yes — they expect this                          |
| General users            | ~Meh — "Press OK to enroll key" is confusing      |
| Enterprise / kiosk       | ❌ No — needs fully automated, no user interaction |

---

## 4. Resolution Auto-Detection

### 4.1 GOP Mode Negotiation

**Prompt:** The bootloader currently hardcodes 1280×720 and falls back to the current
GOP mode. This works universally in QEMU, VirtualBox, and on real hardware. Do NOT use
"pick the highest GOP mode" — OVMF in emulators exposes large preset modes with
`FrameBufferBase = 0` that cause a black screen. The correct real-hardware approach is
**EDID-first selection**, which naturally degrades to stable behaviour in emulators:

```
1. Read EFI_EDID_ACTIVE_PROTOCOL → preferred native resolution (W×H)
2. Find the GOP mode matching that resolution exactly (32bpp)
3. SetMode → verify FrameBufferBase ≠ 0
4. Fall back: search for 1280×720 (or current mode) if EDID fails/no match
5. Pass actual resolution + framebuffer to kernel via boot params (already done)
```

**Why EDID works on both platforms:**
- Real hardware: EDID gives the panel's native resolution → correct match
- QEMU/VirtualBox: `EFI_EDID_ACTIVE_PROTOCOL` typically returns nothing → falls
  through to the 1280×720 fallback cleanly

After completing all items, mark every item as `[x]`, update this prompt to a
verification prompt, run `bash scripts/build.sh clean`, and commit as
`"boot: EDID-based GOP resolution auto-detection"`. Update `README.md` and add notes
directly in this TODO section covering the EDID protocol, GOP mode matching, and the emulator fallback.

> [!NOTE]
> `fb_init()` already reads `g_boot_info.fb.{width,height,pitch}` dynamically —
> no kernel changes are needed for any resolution, only the bootloader changes.

- [ ] Locate `EFI_EDID_ACTIVE_PROTOCOL` via `gBS->LocateProtocol()`
- [ ] Parse the 18-byte preferred timing descriptor (bytes 54–71): extract H-active and V-active pixels
- [ ] Search GOP modes for an exact (W×H, 32bpp) match
- [ ] Call `gop->SetMode()` and verify `FrameBufferBase ≠ 0`
- [ ] Fall back to 1280×720 search if EDID unavailable or no matching GOP mode
- [ ] Final fallback: use current mode (as today)
- [ ] Log selected resolution to UEFI console: `[GOP] WxH 32bpp (EDID/fallback)`
- [ ] Pass resolution + framebuffer to kernel via boot params (already wired up)
- [ ] Commit: `"boot: EDID-based GOP resolution auto-detection"`

### 4.2 HiDPI / Retina Scaling

**Prompt:** On a 4K 14" laptop, 5px dots and 16px text are microscopic. Calculate a DPI scaling factor based on resolution: 1× for ≤1080p, 2× for >1080p and ≤2160p, 3× for >2160p. Scale all boot splash elements (icon size, dot radius, dot spacing, font size, layout offsets) by this factor. Use the scaling factor in the boot params so the kernel desktop can also use it. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"boot: HiDPI scaling for boot splash"`. Update `README.md` if it contains stale or incorrect references to display scaling or DPI. Add notes, gotchas, and design decisions directly in this TODO section covering the HiDPI scaling system, scale factor calculation, and per-element scaling rules.

- [ ] Calculate scale factor: `scale = (height > 2160) ? 3 : (height > 1080) ? 2 : 1`
- [ ] Scale boot splash constants: `DOT_MIN_R`, `DOT_MAX_R`, `DOT_SPACING`, font size
- [ ] Scale icon rendering (use larger icon at 2×/3×, or upscale)
- [ ] Scale text clear area and layout offsets
- [ ] Pass scale factor to kernel for desktop UI scaling
- [ ] Commit: `"boot: HiDPI scaling for boot splash"`

---

## 5. Boot UX Polish

### 5.1 Fade-In Transition

**Prompt:** ✅ VERIFICATION — Fade-in transition is implemented. Verify:
(1) `boot_splash_init()` in `src/kernel/boot_splash.c` calls the fade loop before registering the PIT callback;
(2) `splash_draw_icon_faded(uint8_t fade)` and `splash_draw_dots_faded(uint8_t fade)` exist and scale all color channels by `fade/255`;
(3) The fade loop iterates 5 frames with `fade_levels = {51,102,153,204,255}`, calling `sleep_ms(100)` between each, totalling ~500ms;
(4) `bash scripts/build.sh clean` produces `=== BUILD OK ===`;
(5) QEMU shows a gradual brighten-in rather than a snap.

- [x] Added `splash_draw_icon_faded()` in `boot_splash.c` (replaces original `splash_draw_icon()`)
- [x] Added `splash_draw_dots_faded()` in `boot_splash.c`
- [x] Fade loop: icon + dots at 20% → 40% → 60% → 80% → 100% brightness via `{51,102,153,204,255}`
- [x] Each frame: `sleep_ms(100)` delay + `fb_swap()` — complete before dot animation starts
- [x] Commit: `"boot: fade-in transition for splash screen"`

### 5.2 Boot Profiling via Serial Timestamps

**Prompt:** ✅ VERIFICATION — Serial log timestamps are implemented. Verify:
(1) Every serial `klog()` line in QEMU's `-serial stdio` is prefixed with `[%4u.%03u]` (e.g., `[   1.230] [OK] boot: ...`);
(2) `src/kernel/klog.c` computes `ms = pit_get_ticks() * 10`, then emits space-padded seconds + zero-padded milliseconds;
(3) `main.c` contains `klog(LOG_DEBUG, "boot", "--- Phase: ...")` markers before: storage/VFS, interrupt controllers, display/splash, PCI/network hardware, partition mount, network (DHCP), and desktop/WM;
(4) A `klog(LOG_INFO, "boot", "Boot complete in %u.%03us ...")` appears just before the compositor loop;
(5) `bash scripts/build.sh clean` produces `=== BUILD OK ===`.

- [x] Timestamp prefix `[%4u.%03u]` added to all serial klog() output in `klog.c`
- [x] No separate `boot_timestamp()` needed — logic is inline in `klog()` serial section
- [x] Phase markers in `main.c`: storage/VFS, IDT/PIT, display/splash, PCI, partitions, DHCP, desktop/WM
- [x] `klog(LOG_INFO, "boot", "Boot complete in X.XXXs ...")` before compositor loop
- [x] Commit: `"boot: serial log timestamps for boot profiling"`

### 5.3 Error Recovery Screen

**Prompt:** ✅ VERIFICATION — Boot error recovery screen is implemented in `src/kernel/panic.c`. Verify:
(1) `panic_screen()` calls `if (boot_splash_active()) boot_splash_finish()` at the very start (before `cli`);
(2) Screen is filled with `PANIC_BG_COLOR` (0x00003380 — deep Impossible-OS blue);
(3) BSOD icon is drawn via `draw_bsod_icon()` using the embedded `bsod_icon_pixels` bitmap;
(4) Displayed text includes: title ("Your Impossible OS ran into a problem..."), stop code, description, source file:line, full register dump (RAX–RSP, CR2, CR3), stack trace;
(5) Crash dump written to `C:\Impossible\System\crashdump.log` via VFS;
(6) 30-second auto-restart countdown with progress bar (configurable via Registry `SYSTEM\Recovery\AutoRestart`);
(7) Restart attempted via ACPI reset (port 0xCF9), then triple-fault fallback;
(8) `bash scripts/build.sh clean` produces `=== BUILD OK ===`.

- [x] In `panic()`, check if `boot_splash_active()` is true → line 301 of `panic.c`
- [x] Clear screen to dark blue (`PANIC_BG_COLOR = 0x003380`), render BSOD icon and error text
- [x] Show: title, stop code, description, register dump (all GPRs + CR2/CR3 + CS/SS), stack trace
- [x] 30s countdown with progress bar; restart via ACPI (0xCF9) or triple fault; crash dump to disk
- [x] Commit: `"boot: error recovery screen during splash"`

### 5.4 Parallel Init (Boot Time Optimization)

**Prompt:** ✅ VERIFICATION — Parallel boot init implemented. Verify:
(1) `dhcp_discover()` is called in `main.c` immediately after `sti()` + `boot_splash_start_animation()`, BEFORE `partition_scan_all()`;
(2) A comment block `/* ═══ PARALLEL BOOT ... */` explains the fire-and-forget semantics;
(3) Serial log shows `--- Phase: network (DHCP, async fire-and-forget) ---` BEFORE `--- Phase: partition & filesystem mount ---`;
(4) `docs/architecture/boot-parallel-init.md` exists with dependency graph, profiled stages, and VFS-locking roadmap;
(5) `bash scripts/build.sh clean` produces `=== BUILD OK ===`.

Note: `icon_store_init()` ∥ `cursor_init()` parallelization deferred — VFS/FAT32 has no locking. Enabling it requires adding a `vfs_lock()`/`vfs_unlock()` reader-writer mutex first (see doc for pattern).

- [x] Profiled boot stages via serial timestamps — DHCP round-trip was on critical path
- [x] Dependency graph in `docs/architecture/boot-parallel-init.md`
- [x] DHCP ∥ partition scan: `dhcp_discover()` moved before `partition_scan_all()` — ~300ms savings
- [x] Icon store ∥ cursor: deferred (VFS not thread-safe; doc contains future implementation pattern)
- [x] Commit: `"boot: parallel initialization for faster boot"`

### 5.5 Boot Menu (Recovery Mode)

**Prompt:** Hold Shift or F8 during boot to enter a recovery menu. The boot menu uses the same visual style as the splash (black bg, Selawik font, centered layout) and offers: Normal Boot, Safe Mode (skip modules), Recovery Console (drop to shell), Reboot. Use UEFI `SimpleTextInputEx` protocol to detect keypress during the first 2 seconds after firmware handoff. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"boot: recovery boot menu"`. Update `README.md` if it contains stale or incorrect references to boot modes or recovery options. Add notes, gotchas, and design decisions directly in this TODO section covering the recovery boot menu, key detection, available boot modes, and the user-facing menu UI.

- [ ] Detect Shift/F8 keypress in UEFI bootloader (before ExitBootServices)
- [ ] If key held: set `boot_mode = RECOVERY` in boot params
- [ ] In kernel: if `boot_mode == RECOVERY`, show boot menu instead of splash
- [ ] Menu options:
  - [ ] Normal Boot — continue as usual
  - [ ] Safe Mode — skip loadable modules
  - [ ] Recovery Console — boot to shell, no desktop
  - [ ] Reboot — firmware reset
- [ ] Render menu with embedded TTF font and keyboard navigation
- [ ] Commit: `"boot: recovery boot menu"`

---

## 6. Advanced (Future)

### 6.1 Measured Boot (TPM)

**Prompt:** Implement TPM Measured Boot so each boot stage is hashed into TPM PCR registers, enabling tamper detection and future remote attestation. Realistic scope is **Tier 2** (extend PCRs with kernel hash in `bootx64.c`). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"boot: TPM measured boot (Tier 2 — PCR extend)"`. Add notes directly in this TODO section covering the PCR layout, measurement chain, testing procedure, and the Tier 3 remote attestation roadmap.

**Trust chain:**
`CPU → Firmware (PCR0–7) → UEFI Secure Boot (PCR8–9) → BOOTX64.EFI (PCR4, done by firmware) → kernel.elf (PCR8, done by us)`

Each stage hashes the next and **extends** a PCR — an irreversible accumulative operation. The resulting PCR value fingerprints the exact software that ran. You can't fake it without physical TPM access.

**Tier breakdown (Tier 2 is the target):**

| Tier | What | Effort | Value |
|---|---|---|---|
| 1 — Log PCRs | Read PCR values after boot, log to serial | Low | Audit: "did our boot change?" |
| **2 — Extend PCRs** | Hash `kernel.elf` before load, call `TCG2->HashLogExtendEvent()` | **Medium** | **Actual measured boot — target** |
| 3 — Remote attestation | TPM signs PCRs, sends to remote verifier | Very high | Requires attestation server + crypto |

Tier 3 is out of scope — Windows spent years building it (vTPM, Azure Attestation). PCRs extended in Tier 2 are already available for future attestation.

**Graceful degradation (mandatory):**
```c
EFI_TCG2_PROTOCOL *tcg2 = NULL;
EFI_STATUS s = gBS->LocateProtocol(&gEfiTcg2ProtocolGuid, NULL, (void**)&tcg2);
if (EFI_ERROR(s)) { tcg2 = NULL; /* no TPM — skip silently, boot normally */ }
```
If no TPM is found, boot continues with zero measurement. This handles: QEMU without swtpm, old hardware, VMs without virtual TPM. Works exactly like our VBE page-flip probe.

**QEMU testing (optional — requires swtpm on host):**
```bash
swtpm socket --tpmstate dir=/tmp/tpm --ctrl type=unixio,path=/tmp/tpm.sock --tpm2 &
qemu-system-x86_64 ... -tpmdev emulator,id=tpm0,chardev=chrtpm -device tpm-tis,tpmdev=tpm0
```
Alternative: VirtualBox ≥ 6.1 supports virtual TPM 2.0 (Settings → System → TPM 2.0) — no swtpm needed.

> **Constraints:**
> - All TPM calls in UEFI phase (before `ExitBootServices()`) — Boot Services guaranteed
> - Graceful skip if `EFI_TCG2_PROTOCOL` not found — no crash, no hang
> - Only extend PCR 8 with kernel hash; do not touch PCR 0–7 (firmware-owned)
> - PCR extend is irreversible per boot — don't extend twice (gate on `tcg2 != NULL`)

- [ ] In `bootx64.c`: probe for `EFI_TCG2_PROTOCOL`; if absent, set `tcg2 = NULL` and skip silently
- [ ] If TPM found: compute SHA-256 of `kernel.elf` buffer before jumping to kernel
- [ ] Call `tcg2->HashLogExtendEvent()` to extend PCR 8 with kernel hash
- [ ] Optionally extend PCR 9 with hash of boot parameters / config  
- [ ] Log PCR 4 and PCR 8 values to serial at boot for audit (`[OK] TPM PCR[8]: XXXX...`)
- [ ] Commit: `"boot: TPM measured boot (Tier 2 — PCR extend)"`

### 6.2 UEFI Boot Manager Entry

**Prompt:** Register Impossible OS as a permanent UEFI boot entry so it appears in the firmware boot menu alongside Windows, Linux, and other OSes. The entry lives in NVRAM (not on disk) so it survives disk reformats.

**Architecture decision (confirmed):**
- **Registration** happens in `bootx64.c` **before `ExitBootServices()`** — all Boot Services are fully available here, eliminating any Runtime Services support risk. Idempotent: scans existing `Boot####` entries and skips if already registered. NVRAM is written exactly once, on the very first boot after install.
- **Removal** happens from within the running kernel via UEFI Runtime Services `SetVariable(DataSize=0)` — called only when the user explicitly runs a removal command (e.g. `bootmgr --remove` in the shell). Not called on every boot. Risk profile matches what Windows, GRUB, and systemd-boot all do.

After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"boot: self-register UEFI boot entry"`. Add notes directly in this TODO section covering the boot entry format, registration flow, removal procedure, and `efibootmgr` manual fallback.

> **Constraints:**
> - Only meaningful on real hardware — QEMU's OVMF NVRAM is per-session and not preserved
> - Do NOT call `efibootmgr` from WSL2 — WSL2 has no access to host UEFI NVRAM
> - Registration is idempotent: scan `Boot0000`–`BootFFFF`, skip if "Impossible OS" already found — NVRAM written exactly once
> - Append to `BootOrder` (read → append → write); never overwrite or reorder existing entries
> - Removal must delete `Boot####` variable AND remove its slot from `BootOrder` (dangling `BootOrder` entries cause firmware warnings)
> - A dangling entry (files deleted, NVRAM entry left) is cosmetically bad but not dangerous — firmware skips unbootable entries

- [ ] In `bootx64.c`, before `ExitBootServices()`: scan `Boot####` NVRAM vars for an existing "Impossible OS" description
- [ ] If not found: write new `Boot####` `EFI_LOAD_OPTION` pointing to `\EFI\ImpossibleOS\BOOTX64.EFI` on current EFI partition
- [ ] Read `BootOrder`, append new slot, write back (validate before writing to avoid malformed variable)
- [ ] Kernel shell command `bootmgr --remove`: call UEFI Runtime `SetVariable(Boot####, NULL, 0)` + remove slot from `BootOrder`
- [ ] Manual fallback: document `efibootmgr -b XXXX -B` for users who delete the OS without using the removal tool
- [ ] Commit: `"boot: self-register UEFI boot entry"`


---

## 7. Missing Features vs Windows Boot Manager & GRUB

---

### 7.1 Boot Configuration File

**Prompt:** Windows Boot Manager reads BCD (Boot Configuration Data); GRUB reads `grub.cfg`; systemd-boot reads `loader.conf`. Impossible OS has no equivalent — boot parameters are hardcoded. Add `\EFI\ImpossibleOS\boot.conf` — a simple `key=value` ini file read by `bootx64.c` before loading the kernel. Configurable: kernel path, kernel cmdline, splash timeout, default boot mode, serial debug on/off. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"boot: boot.conf configuration file"`. Add notes directly in this TODO section.


- [ ] Define `boot.conf` ini format: `key=value`, `#` comments, blank lines ignored
- [ ] Implement `parse_boot_conf()` in `bootx64.c` — read from EFI partition via `EFI_SIMPLE_FILE_SYSTEM`
- [ ] Parse keys: `kernel=`, `cmdline=`, `splash_timeout=`, `boot_mode=`, `serial_debug=`
- [ ] Pass parsed cmdline string to kernel in boot params
- [ ] Graceful fallback: if `boot.conf` absent, use hardcoded defaults
- [ ] Ship a default `boot.conf` in the ISO
- [ ] Commit: `"boot: boot.conf configuration file"`

---

### 7.2 Multi-OS Detection & Boot Menu

**Prompt:** Windows Boot Manager auto-detects other OSes on the disk (Linux EFI entries, other Windows installs). GRUB has `os-prober`. Impossible OS currently shows no other OSes. Scan EFI partition for known bootloaders at boot, and if others are found, offer a timed boot menu. Detection: search for `\EFI\Microsoft\Boot\bootmgfw.efi` (Windows), `\EFI\ubuntu\grubx64.efi`, `\EFI\fedora\grubx64.efi`, etc. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"boot: multi-OS detection and boot menu"`. Add notes directly in this TODO section.


- [ ] Scan root EFI partition `\EFI\` subdirectories for `*.efi` files
- [ ] Identify known bootloaders by path (Windows, Ubuntu, Fedora, etc.)
- [ ] If other OSes found AND hold-key not pressed: show timed boot menu (5s default)
- [ ] Menu: highlight default entry, keyboard navigation (↑↓ + Enter), timeout countdown
- [ ] If no other OS found: skip menu entirely, boot immediately
- [ ] Chain-load selected EFI binary via `LoadImage` + `StartImage`
- [ ] Store default OS preference in `boot.conf` (§7.1)
- [ ] Commit: `"boot: multi-OS detection and boot menu"`

---

### 7.3 A/B (Dual-Slot) Boot

**Prompt:** Android, ChromeOS, and modern embedded Linux systems use A/B dual-slot boot — two complete OS copies, updated alternately so a failed update never bricks the device. Boot slot A normally; on consecutive boot failures, auto-switch to slot B (last known good). This is a **major differentiator** — Windows and desktop Linux do NOT have native A/B boot. Store the active slot and failure counter in UEFI NVRAM. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"boot: A/B dual-slot boot"`. Add notes directly in this TODO section.

> **Beats:** Windows 11 (no A/B), desktop Linux (no A/B natively — only Atomic/immutable distros)


- [ ] Define NVRAM variable `ImpossibleOS_BootSlot` (A or B) and `ImpossibleOS_BootFailCount`
- [ ] On boot: read active slot; load kernel from `\boot\kernel_A.exe` or `\boot\kernel_B.exe`
- [ ] On successful boot: kernel resets `BootFailCount = 0` via UEFI Runtime SetVariable
- [ ] On failure (3 consecutive boots without reset): auto-switch slot, reset counter
- [ ] Kernel shell command: `bootslot --swap` to manually switch active slot
- [ ] Updater writes new kernel to inactive slot before swapping (safe update path)
- [ ] Commit: `"boot: A/B dual-slot boot"`

---

### 7.4 Firmware Compatibility Check

**Prompt:** At boot, validate that the firmware meets minimum requirements before loading the kernel. Check: UEFI version ≥ 2.5 (required for EFI_GRAPHICS_OUTPUT_PROTOCOL v2), available RAM ≥ 256 MiB, x86-64 CPU (already guaranteed by EFI mode), and GPU framebuffer accessible (FrameBufferBase ≠ 0 after GOP SetMode). On failure, print a human-readable UEFI console error and halt rather than showing a confusing crash later. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"boot: firmware compatibility check"`. Add notes directly in this TODO section.


- [ ] Check UEFI revision: `gST->Hdr.Revision >= EFI_2_50_SYSTEM_TABLE_REVISION`
- [ ] Check available RAM ≥ 256 MiB from memory map (sum `EfiConventionalMemory` entries)
- [ ] Check GOP framebuffer: `FrameBufferBase != 0` after `SetMode()`
- [ ] On any failure: print diagnostic to UEFI console, call `gBS->Exit()` cleanly
- [ ] Pass firmware version string to kernel in boot params for display in system info
- [ ] Commit: `"boot: firmware compatibility check"`

---

### 7.5 Auto-Recovery After Crash Dump

**Prompt:** When the kernel writes a crash dump (see §5.3 / `panic.c`), the next boot should detect it and offer to: send the dump, view it, or clear it and boot normally. Windows does this automatically (Windows Error Reporting). Linux has `kdump` + `makedumpfile`. Impossible OS already writes `C:\Impossible\System\crashdump.log` — we just need the bootloader to detect it and adjust boot behaviour. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"boot: crash dump detection and recovery menu"`. Add notes directly in this TODO section.

> **Note:** The bootloader cannot read the FAT32 system disk — check is done in early kernel before the desktop, after VFS mounts.


- [ ] In `main.c` early boot (after VFS mount, before desktop): check for `C:\Impossible\System\crashdump.log`
- [ ] If found: show recovery screen with options: View dump / Send to Rizonesoft / Clear and boot normally
- [ ] "View dump" → open crash log in a minimal text viewer (no desktop needed)
- [ ] "Send" → HTTP POST to crash reporting endpoint (requires network up)
- [ ] "Clear" → delete log, boot normally
- [ ] Store crash count in registry `SYSTEM\CrashDump\Count` — auto-enter Safe Mode after 3 consecutive crashes
- [ ] Commit: `"boot: crash dump detection and recovery menu"`

---

## Priority Order

| Priority | Section                          | Description                                                       |
|----------|----------------------------------|-------------------------------------------------------------------|
| ✅ Done   | 1.1–1.5 Custom UEFI Bootloader   | Boot application, GOP, ELF loader, ACPI, page tables              |
| ✅ Done   | 2.1–2.5 Boot Splash              | Splash screen, dots, icon, TTF font, status messages              |
| ✅ Done   | 3.1 Generate MOK Key Pair        | RSA-2048 MOK key generated, committed                             |
| ✅ Done   | 3.2 Sign Bootloader with MOK     | sbsigntool signing wired into build.sh                            |
| ✅ Done   | 3.3 Build and Package Shim       | shim built from rhboot/shim v16.1                                 |
| ✅ Done   | 5.1 Fade-In Transition           | 5-frame fade-in implemented                                       |
| ✅ Done   | 5.2 Boot Profiling               | Serial timestamps + phase markers in main.c                       |
| ✅ Done   | 5.3 Error Recovery Screen        | Panic screen with BSOD, crash dump, 30s countdown                 |
| ✅ Done   | 5.4 Parallel Init                | DHCP fire-and-forget before partition scan                        |
| 🔴 P0     | 4.1 GOP Mode Negotiation         | Without this, splash wrong on any non-720p display                |
| 🟠 P1     | 7.1 Boot Config File             | `boot.conf` — stop hardcoding kernel path and cmdline             |
| 🟠 P1     | 3.5 Interim Pre-Signed Shim      | Ship Secure Boot now (with MOK enrollment)                        |
| 🟠 P1     | 5.5 Boot Menu                    | Recovery — safe mode, console                                     |
| 🟠 P1     | 7.4 Firmware Compatibility Check | Fail fast with clear error instead of cryptic crash               |
| 🟠 P1     | 7.5 Crash Dump Auto-Recovery     | Detect and offer options after panic                              |
| 🟡 P2     | 4.2 HiDPI Scaling                | Required for 4K laptops                                           |
| 🟡 P2     | 7.2 Multi-OS Detection           | Detect other OSes, show timed boot menu                           |
| 🟡 P2     | 6.2 Measured Boot (TPM)          | Security — attestation                                            |
| 🟡 P2     | 6.3 UEFI Boot Manager Entry      | UX — permanent firmware boot menu entry                           |
| 🟢 P3     | 3.4 Submit for Microsoft Signing | Eliminate MOK enrollment for end users                            |
| 🔵 Future | 7.3 A/B Dual-Slot Boot           | Impossible OS differentiator — safe updates, beats Win11 + Linux  |

---

## OS Comparison

| Feature                           | Windows Boot Manager       | GRUB / systemd-boot        | Impossible OS                         |
|-----------------------------------|----------------------------|----------------------------|---------------------------------------|
| Custom UEFI boot application      | ✅ `bootmgfw.efi`          | ✅ `grubx64.efi`           | ✅ §1 Done                            |
| Boot splash screen                | ✅ Windows 11 spinner      | ⚠️ Basic text / theme      | ✅ §2 Done — animated dots, TTF font  |
| Secure Boot (signed)              | ✅ Microsoft CA            | ✅ Distro shim             | ✅ §3 Done — MOK + shim pending MSFT  |
| EDID resolution auto-detect       | ✅                         | ✅ GRUB modes              | ⬜ §4.1 P0                            |
| HiDPI / Retina scaling            | ✅                         | ⚠️ Limited                 | ⬜ §4.2 P2                            |
| Fade-in transition                | ✅ Smooth                  | ❌                          | ✅ §5.1 Done                          |
| Boot profiling / timestamps       | ✅ ETW traces              | ⚠️ Serial only             | ✅ §5.2 Done — serial phase markers   |
| Error recovery screen (BSOD)      | ✅ BSOD + WinRE            | ❌                          | ✅ §5.3 Done                          |
| Parallel init                     | ✅ Parallel service start  | ❌                          | ✅ §5.4 Done — async DHCP             |
| Recovery boot menu (F8/Shift)     | ✅ WinRE                   | ✅ GRUB menu               | ⬜ §5.5 P1                            |
| Measured Boot / TPM               | ✅ Full TPM 2.0            | ✅ GRUB TPM                | ⬜ §6.2 P2                            |
| UEFI boot manager entry           | ✅ Automatic               | ✅ `grub-install`          | ⬜ §6.3 P2                            |
| Boot config file                  | ✅ BCD store               | ✅ `grub.cfg`              | ⬜ §7.1 P1                            |
| Multi-OS detection                | ✅ BCD auto-detect         | ✅ `os-prober`             | ⬜ §7.2 P2                            |
| Firmware compatibility check      | ✅ (implicit)              | ❌                          | ⬜ §7.4 P1 — **Impossible OS only**   |
| Crash dump auto-recovery          | ✅ WER + WinRE             | ❌                          | ⬜ §7.5 P1                            |
| **A/B dual-slot boot**            | ❌                         | ❌ (only Atomic OSes)       | ⬜ **§7.3 Future — beats both**       |
| **Anti-aliased TTF boot font**    | ✅                         | ⚠️ Bitmap fonts            | ✅ **Done — Selawik Semibold 16px**   |
| **Boot profiling serial log**     | ❌ (ETW only, no serial)   | ❌                          | ✅ **Done — beats both**              |

> **After P0+P1 items:** Impossible OS matches Windows Boot Manager feature-for-feature on single hardware.
> **After A/B boot (§7.3):** Exceeds both Windows 11 and desktop Linux — a differentiator unique to Impossible OS.

---

## Boot Splash Sizing Reference

> **Rule of thumb:** Logo ≈ 8% of screen height. Embed a single 256×256 source image
> and scale down at runtime based on detected GOP resolution.

### Element Sizes by Resolution

| Resolution        | Scale | Logo Size | Dot Radius (min/max) | Dot Spacing | Font Size |
|-------------------|-------|-----------|----------------------|-------------|-----------|
| 1280×720 (720p)   | 1×    | 96×96     | 5px / 8px            | 20px        | 16px      |
| 1366×768          | 1×    | 96×96     | 5px / 8px            | 20px        | 16px      |
| 1920×1080 (1080p) | 1×    | 128×128   | 6px / 10px           | 24px        | 18px      |
| 2560×1440 (1440p) | 2×    | 192×192   | 10px / 16px          | 40px        | 32px      |
| 3840×2160 (4K)    | 2×    | 256×256   | 12px / 18px          | 48px        | 36px      |
| 3840×2400 (4K+)   | 3×    | 288×288   | 15px / 22px          | 60px        | 48px      |

### Scale Factor Formula

```
scale = (height > 2160) ? 3 : (height > 1080) ? 2 : 1
logo  = screen_height / 8      (clamped 64–256px)
dots  = 5 * scale / 8 * scale  (min/max radius)
font  = 16 * scale              (Selawik Semibold)
```

### Implementation Strategy

- Embed one **256×256** BGRA boot logo (256 KB)
- Scale down via nearest-neighbor at runtime based on GOP mode
- Same approach as Windows 11: single high-res source, runtime downsample

