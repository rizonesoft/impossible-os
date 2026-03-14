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

**Prompt:** This section is marked complete. Verify the implementation is correct: review `src/boot/uefi/bootx64.c` to confirm it implements a PE/COFF binary at `\EFI\BOOT\BOOTX64.EFI` that replaces GRUB. Confirm it uses the `ms_abi` calling convention, initializes the UEFI system table and boot services, disables the watchdog timer, and passes control to the kernel. Check that the Makefile builds `BOOTX64.EFI` as a PE binary and copies it to the EFI staging directory. Confirm the commit `"boot: custom UEFI bootloader replaces GRUB"` exists in git history. Run `bash scripts/build.sh clean` and verify the ISO boots in QEMU. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to the UEFI bootloader or GRUB. Create or update documentation in `docs/` covering the custom UEFI boot application architecture.

- [x] Create `src/boot/uefi/bootx64.c` — UEFI boot application entry point (`efi_main`)
- [x] Implement `ms_abi` calling convention for UEFI compatibility
- [x] Initialize `EFI_SYSTEM_TABLE`, `EFI_BOOT_SERVICES`, `EFI_HANDLE`
- [x] Disable UEFI watchdog timer (default 5-minute timeout)
- [x] Create `src/boot/uefi/efi.h` — UEFI type definitions and protocol GUIDs
- [x] Build as PE/COFF binary (`BOOTX64.EFI`) via Makefile
- [x] Commit: `"boot: custom UEFI bootloader replaces GRUB"` (`32a5f8f`)

### 1.2 GOP Framebuffer Initialization

**Prompt:** This section is marked complete. Verify that `init_gop()` in `bootx64.c` locates the `EFI_GRAPHICS_OUTPUT_PROTOCOL`, sets the video mode to 1280×720×32bpp with `PixelBlueGreenRedReserved8BitPerColor` format, and stores the framebuffer address, width, height, and pitch in the boot info struct. Confirm `fill_screen_black()` clears the framebuffer immediately after GOP init for a clean transition. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to GOP or framebuffer initialization. Create or update documentation in `docs/` covering GOP mode selection and framebuffer setup.

- [x] Implement `init_gop()` — locate `EFI_GRAPHICS_OUTPUT_PROTOCOL` via `LocateProtocol`
- [x] Set video mode to 1280×720×32bpp (`PixelBlueGreenRedReserved8BitPerColor`)
- [x] Store framebuffer address, width, height, pitch in `boot_info.fb`
- [x] Implement `fill_screen_black()` — clear screen after GOP init
- [x] Commit: `"boot: fix UEFI bootloader — ms_abi calling convention + PE reloc + kernel_main lookup"` (`d11f61f`)

### 1.3 Kernel ELF Loader

**Prompt:** This section is marked complete. Verify that `load_kernel()` in `bootx64.c` reads `\boot\kernel.exe` from the EFI partition using `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`, parses the ELF64 header, loads all `PT_LOAD` program headers into memory at their specified physical addresses, and returns the ELF entry point address. Confirm it handles both LOAD segments (code + data) and zeroes BSS. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to kernel loading. Create or update documentation in `docs/` covering the ELF64 kernel loading process.

- [x] Open `\boot\kernel.exe` via `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`
- [x] Parse ELF64 header (verify magic, class, machine `EM_X86_64`)
- [x] Load `PT_LOAD` segments into memory at specified physical addresses
- [x] Zero BSS regions (file size < memory size)
- [x] Return kernel entry point from ELF header
- [x] Commit: `"boot: custom UEFI bootloader replaces GRUB"` (`32a5f8f`)

### 1.4 ACPI RSDP Discovery

**Prompt:** This section is marked complete. Verify that `find_acpi_rsdp()` in `bootx64.c` searches the UEFI configuration table for the ACPI 2.0 GUID (`EFI_ACPI_20_TABLE_GUID`) and falls back to ACPI 1.0 (`EFI_ACPI_TABLE_GUID`). Confirm the RSDP address and ACPI version are stored in boot info for kernel ACPI parsing. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to ACPI discovery. Create or update documentation in `docs/` covering ACPI RSDP discovery and boot info handoff.

- [x] Search UEFI config tables for `ACPI_20_TABLE_GUID` (prefer v2.0)
- [x] Fall back to `ACPI_TABLE_GUID` (v1.0)
- [x] Store RSDP address and version in `boot_info.acpi_rsdp_addr` / `acpi_version`
- [x] Commit: `"boot: custom UEFI bootloader replaces GRUB"` (`32a5f8f`)

### 1.5 Memory Map & Page Tables

**Prompt:** This section is marked complete. Verify that `get_memory_map()` retrieves the UEFI memory map, `fill_memory_map()` converts UEFI memory types to Multiboot2-compatible types, and `setup_page_tables()` creates identity-mapped page tables covering 4 GiB. Confirm `ExitBootServices()` is called correctly (with retry on stale map key). After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to memory mapping or page tables. Create or update documentation in `docs/` covering the UEFI memory map, page table setup, and ExitBootServices flow.

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

**Prompt:** This section is marked complete. Verify that `src/kernel/boot_splash.c` implements a persistent boot splash with: black background, centered icon at ~40% vertical, animated horizontal dots below the icon, and status text below dots. Confirm `boot_splash_init()` locks the compositor to prevent printk output on screen, `boot_splash_finish()` unlocks it and clears the screen for the desktop. Verify the splash is active from `fb_init()` through desktop startup. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to the boot splash. Create or update documentation in `docs/` covering the boot splash architecture and lifecycle.

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

**Prompt:** This section is marked complete. Verify the dot animation in `boot_splash.c` uses PIT timer callbacks at ~14 fps. Confirm the wave-style animation with 6 dots, pulsing radius (min 5px → max 8px), staggered phases, and smooth sine-like easing. Verify `boot_splash_start_animation()` is called after `pit_init() + sti`, and `pit_unregister_callback()` is called on finish. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to the dot animation. Create or update documentation in `docs/` covering the PIT-driven animation system and wave parameters.

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

**Prompt:** This section is marked complete. Verify that `tools/convert_icon.py` reads a PNG from `resources/boot/` and generates `src/kernel/boot_splash_icon.h` containing raw BGRA pixel data as a C array. Confirm the Makefile has a `boot-icon` target with the correct dependency. Verify the current icon is `boot_96.png` (96×96). After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to boot icon embedding. Create or update documentation in `docs/` covering the PNG-to-C-header icon pipeline and supported resolutions.

- [x] Create `tools/convert_icon.py` — PNG to C header converter (BGRA pixel data)
- [x] Implement minimal PNG decoder (IHDR, IDAT, deflate, unfilter)
- [x] Generate `boot_splash_icon.h` with `BOOT_ICON_W`, `BOOT_ICON_H`, `boot_icon_data[]`
- [x] Add Makefile `boot-icon` target: `resources/boot/boot_96.png → boot_splash_icon.h`
- [x] Support alpha-blended icon rendering (BGRA → framebuffer with alpha compositing)
- [x] Multi-resolution source PNGs: `boot_96.png`, `boot_128.png`, `boot_192.png`, `boot_256.png`, `boot_288.png`
- [x] Commit: `"boot: new boot logo (boot_96.png) + updated start menu icon"` (`2adee94`)

### 2.4 Anti-Aliased TTF Font for Status Text

**Prompt:** This section is marked complete. Verify that `tools/convert_boot_font.py` embeds Selawik Semibold (`selawksb.ttf`) as a C header with pre-rasterized glyphs. Confirm the boot splash uses `boot_font_init(16)` for 16px font, `boot_font_measure()` for text width, and `boot_font_render()` for anti-aliased rendering. Verify 1px letter spacing is applied. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to boot fonts. Create or update documentation in `docs/` covering the TTF-to-C-header font pipeline, the boot font API, and anti-aliased rendering.

- [x] Create `tools/convert_boot_font.py` — TTF to C header (pre-rasterized glyphs)
- [x] Embed Selawik Semibold font at 16px size
- [x] Implement `boot_font_init(pixel_size)` — init embedded font data
- [x] Implement `boot_font_measure(text)` — return text width in pixels
- [x] Implement `boot_font_render(text, x, y, color)` — render with anti-aliasing
- [x] Add 1px letter spacing (`BOOT_LETTER_SPACING`)
- [x] Makefile `boot-font` target: `resources/fonts/selawksb.ttf → boot_splash_font_data.h`
- [x] Commit: `"boot: anti-aliased TTF font for splash status text"` (`dbca624`)

### 2.5 Granular Boot Status Messages

**Prompt:** This section is marked complete. Verify that `main.c` calls `boot_splash_status()` with descriptive messages throughout the boot sequence: "Setting up hardware...", "Detecting hardware...", "Detecting drives...", "Configuring network...", "Loading system configuration...", "Preparing desktop...", "Loading fonts...", "Loading resources...", "Almost ready...". Each message should be followed by `boot_splash_tick()`. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to boot status messages. Create or update documentation in `docs/` covering the boot sequence stages and splash status message flow.

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

**Prompt:** While waiting for Microsoft to sign our shim, use Ubuntu's or Fedora's already-signed `shimx64.efi` as an interim solution. This binary is BSD-licensed and freely redistributable. The tradeoff: users will see a one-time MOK enrollment prompt on first boot (because the distro's shim has the distro's key baked in, not ours). This is the same approach used by Arch Linux. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, and commit as `"boot: interim pre-signed shim with MOK enrollment"`. Update `README.md` if it contains stale or incorrect references to Secure Boot or MOK enrollment. Create or update documentation in `docs/` covering the interim shim approach, MOK enrollment user flow, and install guide instructions.

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

**Prompt:** ✅ VERIFICATION — GOP auto-detection is implemented. Verify:
(1) `init_gop()` in `src/boot/uefi/bootx64.c` iterates all modes via `gop->QueryMode()`,
filters to `PixelBlueGreenRedReserved`/`PixelRedGreenBlueReserved`, picks highest pixel count;
(2) `g_boot_info_ptr->fb.{width,height,pitch}` reflect the selected mode's actual resolution;
(3) `fb_init()` reads from `g_boot_info.fb` with no hardcoded resolution;
(4) `docs/architecture/display-scaling.md` exists;
(5) `bash scripts/build.sh clean` produces `=== BUILD OK ===`.
Commit: `ea07ed7`.

- [x] Enumerate all GOP modes via `gop->QueryMode()`
- [x] Filter for 32bpp modes with `PixelBlueGreenRedReserved` / `PixelRedGreenBlueReserved`
- [x] Select highest resolution mode (largest width × height)
- [x] Set selected mode via `gop->SetMode()`
- [x] Pass resolution, stride, and framebuffer base to kernel via boot params
- [x] `fb_init()` already uses boot-passed resolution — no kernel changes needed
- [x] Test: `bash scripts/build.sh clean` → `=== BUILD OK ===` (16.1s)
- [x] Commit: `"boot: auto-detect display resolution via GOP"` (`ea07ed7`)

### 4.2 HiDPI / Retina Scaling

**Prompt:** On a 4K 14" laptop, 5px dots and 16px text are microscopic. Calculate a DPI scaling factor based on resolution: 1× for ≤1080p, 2× for >1080p and ≤2160p, 3× for >2160p. Scale all boot splash elements (icon size, dot radius, dot spacing, font size, layout offsets) by this factor. Use the scaling factor in the boot params so the kernel desktop can also use it. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"boot: HiDPI scaling for boot splash"`. Update `README.md` if it contains stale or incorrect references to display scaling or DPI. Create or update documentation in `docs/` covering the HiDPI scaling system, scale factor calculation, and per-element scaling rules.

- [ ] Calculate scale factor: `scale = (height > 2160) ? 3 : (height > 1080) ? 2 : 1`
- [ ] Scale boot splash constants: `DOT_MIN_R`, `DOT_MAX_R`, `DOT_SPACING`, font size
- [ ] Scale icon rendering (use larger icon at 2×/3×, or upscale)
- [ ] Scale text clear area and layout offsets
- [ ] Pass scale factor to kernel for desktop UI scaling
- [ ] Commit: `"boot: HiDPI scaling for boot splash"`

---

## 5. Boot UX Polish

### 5.1 Fade-In Transition

**Prompt:** The Dell/HP UEFI firmware logo disappears abruptly when our bootloader takes over (black screen snap). Implement a smooth fade-in: start with a black screen and gradually increase brightness of the icon and dots over ~500ms (5 frames at 10fps). This creates a seamless transition from firmware to OS splash. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"boot: fade-in transition for splash screen"`. Update `README.md` if it contains stale or incorrect references to boot transitions. Create or update documentation in `docs/` covering the fade-in transition implementation and timing.

- [ ] Add `splash_fade_in()` function in `boot_splash.c`
- [ ] Render icon + dots at 20% → 40% → 60% → 80% → 100% brightness
- [ ] Each frame: scale all color channels by fade factor and swap
- [ ] Complete fade before starting dot animation
- [ ] Commit: `"boot: fade-in transition for splash screen"`

### 5.2 Boot Profiling via Serial Timestamps

**Prompt:** Add high-resolution timestamps to each boot stage in the serial log. Use `pit_get_ticks()` to calculate milliseconds since boot for each `klog()` message during init. This helps identify which init stage is slowest (font loading? wallpaper decoding? DHCP?) and optimize boot time. Format: `[  1.234] Initializing AHCI...`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"boot: serial log timestamps for boot profiling"`. Update `README.md` if it contains stale or incorrect references to boot logging or profiling. Create or update documentation in `docs/` covering the boot profiling system, timestamp format, and how to interpret serial log timings.

- [ ] Add `boot_timestamp()` helper: returns ms since kernel start
- [ ] Prefix all `klog()` output with `[%6d.%03d]` timestamp
- [ ] Add timing markers in `main.c` for each major init phase
- [ ] Log total boot time: `"Boot complete in X.XXXs"`
- [ ] Commit: `"boot: serial log timestamps for boot profiling"`

### 5.3 Error Recovery Screen

**Prompt:** If the kernel panics during the boot splash phase, show a clean error screen instead of hanging or showing garbled output. Detect panics via the existing `panic()` handler — if `boot_splash_active()` is true, render a BSOD-style error screen using the embedded TTF font. Show the panic message, register dump, and a "Press any key to restart" prompt. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"boot: error recovery screen during splash"`. Update `README.md` if it contains stale or incorrect references to error handling or panics. Create or update documentation in `docs/` covering the boot error recovery screen, panic detection during splash, and the user-facing error display.

- [ ] In `panic()`, check if `boot_splash_active()` is true
- [ ] If active: clear screen to dark blue, render error text with boot_font_render
- [ ] Show: "Impossible OS encountered an error", panic message, register dump
- [ ] Wait for keyboard interrupt or 30-second timeout, then reboot
- [ ] Commit: `"boot: error recovery screen during splash"`

### 5.4 Parallel Init (Boot Time Optimization)

**Prompt:** Reduce boot time by overlapping slow I/O operations. While fonts are loading from disk, other non-dependent subsystems can initialize. Identify the dependency graph of init stages and parallelize independent branches. Candidate: start DHCP negotiation (which has network round-trip latency) concurrently with font loading. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"boot: parallel initialization for faster boot"`. Update `README.md` if it contains stale or incorrect references to boot time or initialization order. Create or update documentation in `docs/` covering the parallel init system, dependency graph, and measured boot time improvements.

- [ ] Profile current boot: identify the 3 slowest init stages
- [ ] Build dependency graph: which stages can overlap?
- [ ] Candidate parallelizations:
  - [ ] Font loading ∥ DHCP negotiation (both I/O-bound, independent)
  - [ ] Icon store loading ∥ cursor loading
- [ ] Implement via async init tasks (use existing threading if available)
- [ ] Measure before/after boot times
- [ ] Commit: `"boot: parallel initialization for faster boot"`

### 5.5 Boot Menu (Recovery Mode)

**Prompt:** Hold Shift or F8 during boot to enter a recovery menu. The boot menu uses the same visual style as the splash (black bg, Selawik font, centered layout) and offers: Normal Boot, Safe Mode (skip modules), Recovery Console (drop to shell), Reboot. Use UEFI `SimpleTextInputEx` protocol to detect keypress during the first 2 seconds after firmware handoff. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"boot: recovery boot menu"`. Update `README.md` if it contains stale or incorrect references to boot modes or recovery options. Create or update documentation in `docs/` covering the recovery boot menu, key detection, available boot modes, and the user-facing menu UI.

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

### 6.1 Compressed Kernel

- [ ] gzip or lz4 compress the kernel ELF at build time
- [ ] Decompress in the UEFI bootloader after loading from disk
- [ ] Faster disk→RAM transfer (kernel is ~2 MB compressed vs ~8 MB raw)

### 6.2 Measured Boot (TPM)

- [ ] Log each boot stage hash to TPM PCR registers
- [ ] Enables remote attestation: "this machine booted authentic Impossible OS"
- [ ] Requires UEFI TPM protocol access

### 6.3 UEFI Boot Manager Entry

- [ ] Register Impossible OS as a permanent UEFI boot entry via `efibootmgr`
- [ ] Users can select Impossible OS from BIOS boot menu alongside Windows/Linux
- [ ] Survives disk reformats (entry lives in NVRAM, not on disk)

---

## Priority Order

| Priority | Section                          | Description                                              |
|----------|----------------------------------|----------------------------------------------------------|
| ✅ Done   | 1.1–1.5 Custom UEFI Bootloader   | Boot application, GOP, ELF loader, ACPI, page tables     |
| ✅ Done   | 2.1–2.5 Boot Splash              | Splash screen, dots, icon, TTF font, status messages     |
| 🔴 P0     | 4.1 GOP Mode Negotiation         | Without this, splash looks wrong on any non-720p display |
| 🟠 P1     | 3.1 Generate MOK Key Pair        | Foundation for Secure Boot                               |
| 🟠 P1     | 3.2 Sign Bootloader with MOK     | Enables Secure Boot testing                              |
| 🟠 P1     | 3.5 Interim Pre-Signed Shim      | Ship Secure Boot now (with MOK enrollment)               |
| 🟡 P2     | 4.2 HiDPI Scaling                | Required for 4K laptops                                  |
| 🟡 P2     | 5.1 Fade-In Transition           | Polish — smooth firmware→OS transition                   |
| 🟡 P2     | 5.2 Boot Profiling               | Developer tool — identify slow stages                    |
| 🟡 P2     | 5.3 Error Recovery Screen        | UX — clean panic during boot                             |
| 🟢 P3     | 3.3 Build and Package Shim       | Build our own shim from source                           |
| 🟢 P3     | 3.4 Submit for Microsoft Signing | Eliminate MOK enrollment for end users                   |
| 🟢 P3     | 5.4 Parallel Init                | Performance — reduce boot time                           |
| 🟢 P3     | 5.5 Boot Menu                    | Recovery — safe mode, console                            |
| 🔵 P4     | 6.1 Compressed Kernel            | Performance — smaller kernel image                       |
| 🔵 P4     | 6.2 Measured Boot (TPM)          | Security — attestation                                   |
| 🔵 P4     | 6.3 UEFI Boot Manager Entry      | UX — permanent boot menu entry                           |

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

