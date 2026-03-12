# Phase 17 — Bootloader & Secure Boot

> **Goal:** Harden the UEFI bootloader for real-world hardware. Implement Secure Boot
> support via the shim chain-loading approach, sign our bootloader with a Machine Owner
> Key (MOK), and submit our shim to Microsoft for signing. Also improve boot UX with
> resolution auto-detection, fade-in transitions, and boot profiling.

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

## 1. Secure Boot — Shim Chain-Loading

### 1.1 Generate MOK Key Pair

**Prompt:** Generate a Machine Owner Key (MOK) for signing our bootloader. Use `openssl` to create an RSA-2048 key pair with a 10-year validity. The public certificate (`MOK.cer`) will be embedded in our shim build. The private key (`MOK.key`) is used to sign `BOOTX64.EFI` at build time. Store keys in a `keys/` directory (gitignored). Add a `keys/README.md` explaining the key purpose and rotation procedure. After completing all items, mark every item as `[x]`, and commit as `"boot: MOK key generation infrastructure"`.

- [ ] Create `keys/` directory (add to `.gitignore`)
- [ ] Generate MOK key pair:
  ```bash
  openssl req -new -x509 -newkey rsa:2048 -keyout keys/MOK.key \
    -out keys/MOK.cer -days 3650 -nodes \
    -subj "/CN=Impossible OS Secure Boot Key/"
  ```
- [ ] Convert to DER format for UEFI: `openssl x509 -in keys/MOK.cer -out keys/MOK.der -outform DER`
- [ ] Create `keys/README.md` documenting key purpose and rotation
- [ ] Add `keys/MOK.key` to `.gitignore` (NEVER commit private keys)
- [ ] Commit: `"boot: MOK key generation infrastructure"`

### 1.2 Sign Bootloader with MOK

**Prompt:** Use `sbsign` (from `sbsigntool` package) to sign our compiled `BOOTX64.EFI` with the MOK private key. Add a `sign-efi` target to the Makefile that runs after the EFI bootloader is built. The signed binary replaces the unsigned one. Verify the signature with `sbverify`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"boot: sign EFI bootloader with MOK"`.

- [ ] Install `sbsigntool` (system package)
- [ ] Add Makefile target `sign-efi`:
  ```makefile
  sign-efi: uefi-boot
      sbsign --key keys/MOK.key --cert keys/MOK.cer \
        --output build/efi_staging/EFI/BOOT/grubx64.efi \
        build/efi_staging/EFI/BOOT/BOOTX64.EFI
  ```
- [ ] Verify signature: `sbverify --cert keys/MOK.cer build/efi_staging/EFI/BOOT/grubx64.efi`
- [ ] Update `scripts/build.sh` to call `sign-efi` when keys exist
- [ ] If `keys/MOK.key` is missing, skip signing (unsigned dev builds still work)
- [ ] Commit: `"boot: sign EFI bootloader with MOK"`

### 1.3 Build and Package Shim

**Prompt:** Fork the [rhboot/shim](https://github.com/rhboot/shim) project. Embed our `MOK.cer` as the vendor certificate. Build `shimx64.efi` from source. Package the EFI system partition with the correct layout: `shimx64.efi` renamed to `BOOTX64.EFI` (firmware loads this first), our signed bootloader as `grubx64.efi` (shim loads this), and `mmx64.efi` (MokManager for first-boot key enrollment). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"boot: shim packaging with vendor certificate"`.

- [ ] Fork `rhboot/shim` into public `impossible-os-shim` repo
- [ ] Embed our `MOK.cer` as vendor certificate (in `Makefile`: `VENDOR_CERT_FILE`)
- [ ] Build shim: `make VENDOR_CERT_FILE=MOK.cer EFI_PATH=/usr/lib/gnuefi`
- [ ] Package EFI partition:
  ```
  \EFI\BOOT\
    ├── BOOTX64.EFI      ← shimx64.efi (renamed — firmware loads this)
    ├── grubx64.efi       ← Our bootloader (signed with MOK.key)
    └── mmx64.efi         ← MokManager (for first-boot key enrollment)
  ```
- [ ] Test in QEMU with Secure Boot enabled (OVMF + enrolled PK/KEK)
- [ ] Commit: `"boot: shim packaging with vendor certificate"`

### 1.4 Submit Shim for Microsoft Signing

**Prompt:** Submit our shim build to Microsoft for Secure Boot signing via the [rhboot/shim-review](https://github.com/rhboot/shim-review) process. This is free for open-source projects. The shim fork must be in a **public** GitHub repo. Microsoft reviews the shim (not our OS) and signs it with the Microsoft UEFI CA certificate. Once signed, our shim is trusted by all UEFI firmware worldwide — zero MOK enrollment needed for end users. After completing all items, mark every item as `[x]`, and commit as `"boot: Microsoft-signed shim submitted"`.

**Requirements for shim-review submission:**
- [ ] Public GitHub repo with our shim fork
- [ ] Vendor certificate (`MOK.cer`) embedded in shim
- [ ] No modifications to shim code (or clearly documented and justified changes)
- [ ] Reproducer-verified build (reviewers must be able to build identical binary)

**Submission process:**
- [ ] Open an issue on `rhboot/shim-review` with:
  - [ ] Link to our public shim fork
  - [ ] SHA256 hash of the built `shimx64.efi`
  - [ ] Explanation of what Impossible OS is
  - [ ] Build instructions for reproducible verification
- [ ] Respond to Microsoft reviewer feedback (typically 2-4 weeks)
- [ ] Receive signed `shimx64.efi` binary
- [ ] Replace unsigned shim with Microsoft-signed binary in our ISO build
- [ ] Commit: `"boot: Microsoft-signed shim submitted"`

**What must be public vs private:**

| Component | Must be public? | Reason |
|-----------|----------------|--------|
| Shim fork (tiny first-stage loader) | ✅ Yes | Microsoft reviews this |
| `bootx64.c` (our bootloader) | ❌ No | Shim just checks our MOK signature |
| Kernel source | ❌ No | UEFI/shim never sees the kernel |
| Impossible OS codebase | ❌ No | Completely irrelevant to Secure Boot |

### 1.5 Interim: Ship Pre-Signed Shim (Before Microsoft Signing)

**Prompt:** While waiting for Microsoft to sign our shim, use Ubuntu's or Fedora's already-signed `shimx64.efi` as an interim solution. This binary is BSD-licensed and freely redistributable. The tradeoff: users will see a one-time MOK enrollment prompt on first boot (because the distro's shim has the distro's key baked in, not ours). This is the same approach used by Arch Linux. After completing all items, mark every item as `[x]`, and commit as `"boot: interim pre-signed shim with MOK enrollment"`.

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

| Audience | MOK enrollment acceptable? |
|----------|---------------------------|
| Developers / enthusiasts | ✅ Yes — they expect this |
| General users | ~Meh — "Press OK to enroll key" is confusing |
| Enterprise / kiosk | ❌ No — needs fully automated, no user interaction |

---

## 2. Resolution Auto-Detection

### 2.1 GOP Mode Negotiation

**Prompt:** Our bootloader currently hardcodes 1280×720. Real laptops have 1920×1080, 2560×1440, or 3840×2160 displays. Use UEFI's `EFI_GRAPHICS_OUTPUT_PROTOCOL` to enumerate all available modes and select the best one. Prefer the highest resolution that matches the native display. Pass the actual resolution and framebuffer info to the kernel via the boot info struct. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"boot: auto-detect display resolution via GOP"`.

- [ ] Enumerate all GOP modes via `gop->QueryMode()`
- [ ] Filter for 32bpp modes with `PixelBlueGreenRedReserved8BitPerColor` format
- [ ] Select highest resolution mode (prefer native over scaled)
- [ ] Set selected mode via `gop->SetMode()`
- [ ] Pass resolution, stride, and framebuffer base to kernel via boot params
- [ ] Update kernel `fb_init()` to use boot-passed resolution instead of hardcoded
- [ ] Test: verify correct behavior at 1080p, 1440p, 4K
- [ ] Commit: `"boot: auto-detect display resolution via GOP"`

### 2.2 HiDPI / Retina Scaling

**Prompt:** On a 4K 14" laptop, 5px dots and 16px text are microscopic. Calculate a DPI scaling factor based on resolution: 1× for ≤1080p, 2× for >1080p and ≤2160p, 3× for >2160p. Scale all boot splash elements (icon size, dot radius, dot spacing, font size, layout offsets) by this factor. Use the scaling factor in the boot params so the kernel desktop can also use it. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"boot: HiDPI scaling for boot splash"`.

- [ ] Calculate scale factor: `scale = (height > 2160) ? 3 : (height > 1080) ? 2 : 1`
- [ ] Scale boot splash constants: `DOT_MIN_R`, `DOT_MAX_R`, `DOT_SPACING`, font size
- [ ] Scale icon rendering (use larger icon at 2×/3×, or upscale)
- [ ] Scale text clear area and layout offsets
- [ ] Pass scale factor to kernel for desktop UI scaling
- [ ] Commit: `"boot: HiDPI scaling for boot splash"`

---

## 3. Boot UX Polish

### 3.1 Fade-In Transition

**Prompt:** The Dell/HP UEFI firmware logo disappears abruptly when our bootloader takes over (black screen snap). Implement a smooth fade-in: start with a black screen and gradually increase brightness of the icon and dots over ~500ms (5 frames at 10fps). This creates a seamless transition from firmware to OS splash. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"boot: fade-in transition for splash screen"`.

- [ ] Add `splash_fade_in()` function in `boot_splash.c`
- [ ] Render icon + dots at 20% → 40% → 60% → 80% → 100% brightness
- [ ] Each frame: scale all color channels by fade factor and swap
- [ ] Complete fade before starting dot animation
- [ ] Commit: `"boot: fade-in transition for splash screen"`

### 3.2 Boot Profiling via Serial Timestamps

**Prompt:** Add high-resolution timestamps to each boot stage in the serial log. Use `pit_get_ticks()` to calculate milliseconds since boot for each `klog()` message during init. This helps identify which init stage is slowest (font loading? wallpaper decoding? DHCP?) and optimize boot time. Format: `[  1.234] Initializing AHCI...`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"boot: serial log timestamps for boot profiling"`.

- [ ] Add `boot_timestamp()` helper: returns ms since kernel start
- [ ] Prefix all `klog()` output with `[%6d.%03d]` timestamp
- [ ] Add timing markers in `main.c` for each major init phase
- [ ] Log total boot time: `"Boot complete in X.XXXs"`
- [ ] Commit: `"boot: serial log timestamps for boot profiling"`

### 3.3 Error Recovery Screen

**Prompt:** If the kernel panics during the boot splash phase, show a clean error screen instead of hanging or showing garbled output. Detect panics via the existing `panic()` handler — if `boot_splash_active()` is true, render a BSOD-style error screen using the embedded TTF font. Show the panic message, register dump, and a "Press any key to restart" prompt. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"boot: error recovery screen during splash"`.

- [ ] In `panic()`, check if `boot_splash_active()` is true
- [ ] If active: clear screen to dark blue, render error text with boot_font_render
- [ ] Show: "Impossible OS encountered an error", panic message, register dump
- [ ] Wait for keyboard interrupt or 30-second timeout, then reboot
- [ ] Commit: `"boot: error recovery screen during splash"`

### 3.4 Parallel Init (Boot Time Optimization)

**Prompt:** Reduce boot time by overlapping slow I/O operations. While fonts are loading from disk, other non-dependent subsystems can initialize. Identify the dependency graph of init stages and parallelize independent branches. Candidate: start DHCP negotiation (which has network round-trip latency) concurrently with font loading. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"boot: parallel initialization for faster boot"`.

- [ ] Profile current boot: identify the 3 slowest init stages
- [ ] Build dependency graph: which stages can overlap?
- [ ] Candidate parallelizations:
  - [ ] Font loading ∥ DHCP negotiation (both I/O-bound, independent)
  - [ ] Icon store loading ∥ cursor loading
- [ ] Implement via async init tasks (use existing threading if available)
- [ ] Measure before/after boot times
- [ ] Commit: `"boot: parallel initialization for faster boot"`

### 3.5 Boot Menu (Recovery Mode)

**Prompt:** Hold Shift or F8 during boot to enter a recovery menu. The boot menu uses the same visual style as the splash (black bg, Selawik font, centered layout) and offers: Normal Boot, Safe Mode (skip modules), Recovery Console (drop to shell), Reboot. Use UEFI `SimpleTextInputEx` protocol to detect keypress during the first 2 seconds after firmware handoff. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"boot: recovery boot menu"`.

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

## 4. Advanced (Future)

### 4.1 Compressed Kernel

- [ ] gzip or lz4 compress the kernel ELF at build time
- [ ] Decompress in the UEFI bootloader after loading from disk
- [ ] Faster disk→RAM transfer (kernel is ~2 MB compressed vs ~8 MB raw)

### 4.2 Measured Boot (TPM)

- [ ] Log each boot stage hash to TPM PCR registers
- [ ] Enables remote attestation: "this machine booted authentic Impossible OS"
- [ ] Requires UEFI TPM protocol access

### 4.3 UEFI Boot Manager Entry

- [ ] Register Impossible OS as a permanent UEFI boot entry via `efibootmgr`
- [ ] Users can select Impossible OS from BIOS boot menu alongside Windows/Linux
- [ ] Survives disk reformats (entry lives in NVRAM, not on disk)

---

## Priority Order

| Priority | Section | Description |
|----------|---------|-------------|
| 🔴 P0 | 2.1 GOP Mode Negotiation | Without this, splash looks wrong on any non-720p display |
| 🟠 P1 | 1.1 Generate MOK Key Pair | Foundation for Secure Boot |
| 🟠 P1 | 1.2 Sign Bootloader with MOK | Enables Secure Boot testing |
| 🟠 P1 | 1.5 Interim Pre-Signed Shim | Ship Secure Boot now (with MOK enrollment) |
| 🟡 P2 | 2.2 HiDPI Scaling | Required for 4K laptops |
| 🟡 P2 | 3.1 Fade-In Transition | Polish — smooth firmware→OS transition |
| 🟡 P2 | 3.2 Boot Profiling | Developer tool — identify slow stages |
| 🟡 P2 | 3.3 Error Recovery Screen | UX — clean panic during boot |
| 🟢 P3 | 1.3 Build and Package Shim | Build our own shim from source |
| 🟢 P3 | 1.4 Submit for Microsoft Signing | Eliminate MOK enrollment for end users |
| 🟢 P3 | 3.4 Parallel Init | Performance — reduce boot time |
| 🟢 P3 | 3.5 Boot Menu | Recovery — safe mode, console |
| 🔵 P4 | 4.1 Compressed Kernel | Performance — smaller kernel image |
| 🔵 P4 | 4.2 Measured Boot (TPM) | Security — attestation |
| 🔵 P4 | 4.3 UEFI Boot Manager Entry | UX — permanent boot menu entry |
