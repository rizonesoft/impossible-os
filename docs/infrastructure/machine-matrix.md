<!-- docs: covers=todo/00-infrastructure/TODO-01-developer-tooling-stack.md sources=scripts/machines,scripts/run-qemu.sh,scripts/test-smoke.sh,scripts/debug.sh,scripts/deploy/write-usb.sh,src/kernel/drivers/serial.c reviewed=2026-09-28 -->
# Machine Launcher and Debug Profile Matrix

> **Owner:** [Developer Tooling Stack roadmap -- Machine Launcher and Debug Profile Matrix section](../../todo/00-infrastructure/TODO-01-developer-tooling-stack.md#4-machine-launcher-and-debug-profile-matrix).
> **Audience:** developers picking the right launcher for a specific scenario (CPU/SMP, storage, filesystem, secure-boot, bare metal) without script archaeology.
> **Out of scope:** release-validation VM flows (owner: [Release QA roadmap -- QEMU Validation + VirtualBox Certification](../../todo/15-installer-release/TODO-04-release-qa.md)) and installer/provisioning VM templates (owner: [Unattended Install roadmap -- VM Provisioning Templates](../../todo/15-installer-release/TODO-02-unattended-install.md)). This doc does not duplicate those.

The repository ships one default launcher plus pinned specialist launchers under `scripts/machines/`. Each launcher below is documented with its host, accelerator, artifacts, debugger entry point, and known limitations.

## Default Path

For the common case, prefer the wrappers:

- [`bash scripts/build.sh run`](../../scripts/build.sh) -- build + boot in QEMU. Auto-picks KVM on Linux (if `/dev/kvm` writable), WHPX on Windows via the `.bat` shims, TCG fallback.
- [`bash scripts/run-qemu.sh`](../../scripts/run-qemu.sh) -- launch without rebuild. Flags: `--debug-tests`, `--test-only`, `--single-cpu`, `--debug`, `--headless`, `--help`.
- [`bash scripts/test-smoke.sh`](../../scripts/test-smoke.sh) -- headless build + boot, 30s timeout, asserts `Boot complete in` + `C:\>` on serial. KVM preferred.
- [`bash scripts/debug.sh`](../../scripts/debug.sh) -- launch QEMU with gdb stub on `:1234` AND start GDB with kernel symbols. Default breakpoints: `kernel_main`, `panic`, `page_fault_handler`, `general_protection_fault_handler`, `double_fault_handler`. One-shot launcher; do not use to attach to an already-running VM.

Specialist launchers live under `scripts/machines/` (and one under `scripts/debug/`). Use them when the default path does not match the scenario.

---

## QEMU -- Accelerator and Topology

All QEMU launchers boot `build/system-disk.img` (GPT: EFI + BlackBox + IXFS) and print to stdout on Linux or a Windows console window.

**OVMF_VARS lifecycle.** Default `run-qemu.sh` and Windows `run-qemu.ps1` **preserve** writable EFI variables (`build/OVMF_VARS_4M.fd` or `%TEMP%\OVMF_VARS_4M.fd`) across runs -- delete manually, or use `reset-qemu-nvram.*` on Windows. Forced `run-qemu-kvm.sh` / `run-qemu-tcg.sh` recopy `build/OVMF_VARS_4M.fd` into `/tmp/OVMF_VARS_4M_{kvm,tcg}.fd` on each launch, but the underlying `build/` copy is still stateful; delete it for a pristine run.

**Debugger entry point (all QEMU launchers).** Linux: `bash scripts/debug.sh` combines launch + GDB with kernel symbols. Manual attach: add `-s -S` to the QEMU command line and connect from GDB via `target remote localhost:1234`. Windows: `run-qemu.ps1` accepts `-ExtraArgs '-s -S'` for the same effect.

**Trap behavior caveat.** WHPX silently absorbs unknown MSR reads (returns 0, no `#GP`). KVM and real hardware raise `#GP`, and the kernel's `msr_try_read()` probe catches it. If a bug only reproduces on KVM or bare metal, WHPX is hiding it. See CLAUDE.md "Bare Metal Gotchas."

### [`scripts/run-qemu.sh`](../../scripts/run-qemu.sh) -- Linux / WSL2

- **Accelerator:** auto (KVM when `/dev/kvm` writable, TCG fallback).
- **Use:** default entry point. `--debug` for gdb stub, `--single-cpu` for SMP bisect, `--headless` for CI-like.

### [`scripts/machines/run-qemu-kvm.sh`](../../scripts/machines/run-qemu-kvm.sh) -- Linux / WSL2

- **Accelerator:** KVM (forced; `-enable-kvm -cpu host`).
- **Use:** hardware-accelerated CPUID / LAPIC path. Fails fast if `/dev/kvm` is missing or unwritable.

### [`scripts/machines/run-qemu-tcg.sh`](../../scripts/machines/run-qemu-tcg.sh) -- Linux / WSL2

- **Accelerator:** TCG (forced software emulation).
- **Use:** deterministic, portable; PIT timer path via `platform_is_tcg()`. Slow -- not suitable for fast iteration.

### [`scripts/machines/run-qemu-kvm.bat`](../../scripts/machines/run-qemu-kvm.bat) -- Windows

- **Accelerator:** **WHPX** (despite the name). Dispatches to `run-qemu.ps1 -Accel whpx`.
- **Use:** default Windows interactive launcher.
- **Caveat:** the name is misleading -- this `.bat` does not use KVM. The bash counterpart `run-qemu-kvm.sh` does.

### [`scripts/machines/run-qemu-tcg.bat`](../../scripts/machines/run-qemu-tcg.bat) -- Windows

- **Accelerator:** TCG (forced).
- **Use:** validate the TCG path on Windows without WHPX acceleration.

### [`scripts/machines/run-qemu-1cpu.bat`](../../scripts/machines/run-qemu-1cpu.bat) -- Windows

- **Accelerator:** WHPX, 1 vCPU (`-Smp 1`).
- **Use:** SMP bisect. If a crash reproduces with 2 vCPUs but not 1, it is a concurrency bug.

### [`scripts/machines/run-qemu.ps1`](../../scripts/machines/run-qemu.ps1) -- Windows

- **Accelerator:** `-Accel auto | whpx | tcg`.
- **Use:** underlying PowerShell launcher the `.bat` shims dispatch to. Direct use for `-TestOnly`, `-TestSuite`, `-Quiet`, `-CrashTest`, `-ErrorScreenTest`, `-ExtraArgs`.

---

## Secure Boot

### [`scripts/debug/kernel/run-secureboot.bat`](../../scripts/debug/kernel/run-secureboot.bat) -- Windows

- **Accelerator:** TCG + q35 + SMM + pflash.
- **Use:** shim + MokManager + signed EFI Secure Boot validation. First run: enroll `MOK.cer` via MokManager. Subsequent runs boot with `SecureBoot=1`. **Requires a pinned shim -- none is pinned today** (MS UEFI CA 2011 expired 2026-06-30), so a stock build exercises direct boot, not this flow; restore a chain via `scripts/secure-boot/build-shim.sh` and confirm with `scripts/test-secureboot-smoke.sh`.
- **Artifacts:** `%TEMP%\OVMF_VARS_secureboot.fd` (persists MOK enrollment across runs). Delete to reset.
- **Debugger:** `-s -S` addable via script edit; GDB on SMM paths is tricky -- prefer serial + `llvm-addr2line-19`.
- **Caveat:** WHPX cannot emulate secboot pflash; this launcher must stay on TCG. Lives under `scripts/debug/` due to shim+MOK staging; consolidation under `scripts/machines/` is a known drift.

---

## VirtualBox

Third-party hypervisor validation (GPU / framebuffer, absolute pointer, VBoxSVGA driver). Artifact: `build/system-disk.vdi` (re-derived from `system-disk.img` each run). Requires VirtualBox 7.0+ (AHCI + UEFI). Debugger: no gdb path; use `VBoxManage debugvm ImpossibleOS ...` for the VBox internal debugger.

### [`scripts/machines/run-vbox.sh`](../../scripts/machines/run-vbox.sh) -- Linux / WSL2

- `--headless` for CI-like.
- `--debug` for debug-boot (X:\DEBUG flag injected).

### [`scripts/machines/run-vbox.ps1`](../../scripts/machines/run-vbox.ps1) -- Windows

- `-DebugBoot` shows live boot output instead of splash.

### [`scripts/machines/run-vbox.bat`](../../scripts/machines/run-vbox.bat) -- Windows

- Double-click wrapper for `run-vbox.ps1`.

---

## Storage Scenarios

`-Accel` accepts `auto | whpx | tcg`. Defaults: **NVMe and USB scripts default to `tcg`** because WHPX hangs on xHCI and processes NVMe doorbell MMIO writes asynchronously through its event loop.

**Debugger.** Both storage launchers accept `-ExtraArgs '<string>'`. Append `-s -S` to expose the GDB stub; append additional `-drive ...` / `-device ...` / `-netdev ...` fragments to extend the scenario without editing the script. Paths containing spaces survive via double-quoting inside the string (e.g. `-ExtraArgs '-drive "id=extra,file=C:\Users\Jane Doe\disk.img,format=raw"'`); the launchers use a quote-aware tokenizer rather than a plain space split. The `.bat` shims forward `%*` so `run-nvme-test.bat -ExtraArgs "-s -S"` works from cmd.exe as well.

### [`scripts/machines/storage/run-nvme-test.{ps1,bat}`](../../scripts/machines/storage/) -- Windows

- **Params:** `-Accel auto|whpx|tcg`, `-Build`, `-ExtraArgs '<string>'`.
- **Default accelerator:** `tcg`. Overridable via `-Accel whpx`.
- **Use:** 128 MiB emulated NVMe drive alongside the AHCI system disk.
- **Caveat:** WHPX NVMe admin/I/O timeouts are expected; TCG is the reliable accelerator for NVMe driver runs.

### [`scripts/machines/storage/run-usb-test.{ps1,bat}`](../../scripts/machines/storage/) -- Windows

- **Params:** `-Accel auto|whpx|tcg`, `-Build`, `-ExtraArgs '<string>'`.
- **Default accelerator:** `tcg` (WHPX hangs with xHCI device attached).
- **Use:** xHCI host-controller scenarios.
- **Caveat:** xHCI emulation differs from real hardware in port-reset timing; bare metal remains authoritative.

---

## Filesystem Scenarios

`run-fs-test.ps1` defaults to `-Accel auto`. Test output arrives on serial as `[PASS]` / `[FAIL]` lines. Debugger: `run-fs-test.ps1` accepts `-ExtraArgs '-s -S'`; the `.bat` shims (`run-fat32-test.bat`, `run-ntfs-test.bat`, `run-all-fs-tests.bat`) forward `%*` so the same flag flows through from cmd.exe. `run-all-fs-tests.bat` applies any forwarded `-ExtraArgs` to every filesystem in the sweep.

### [`scripts/machines/fs/run-fs-test.ps1`](../../scripts/machines/fs/run-fs-test.ps1) -- Windows

- **Params:** `-Disk ntfs|fat32|ext2|ext3|ext4|exfat|ixfs|mbr|gpt`, `-Accel`, `-Build`, `-GenDisk`, `-ExtraArgs '<string>'`.
- **Use:** attach a filesystem-specific test disk on AHCI port 1 alongside the system disk. Kernel auto-detects filesystem and runs in-kernel tests when the volume label matches the expected test pattern.

### [`scripts/machines/fs/run-fat32-test.bat`](../../scripts/machines/fs/run-fat32-test.bat) -- Windows

- FAT32 driver scenario.

### [`scripts/machines/fs/run-ntfs-test.bat`](../../scripts/machines/fs/run-ntfs-test.bat) -- Windows

- NTFS driver scenario; auto-generates a 32 MiB test disk via `-GenDisk`.

### [`scripts/machines/fs/run-all-fs-tests.bat`](../../scripts/machines/fs/run-all-fs-tests.bat) -- Windows

- Sequential NTFS / FAT32 / ext2 / ext4 / ixfs sweep. Interactive (close QEMU between scenarios).

---

## EFI Variable Reset

### [`scripts/machines/reset-qemu-nvram.{ps1,bat}`](../../scripts/machines/) -- Windows

- Deletes `%TEMP%\OVMF_VARS_4M.fd` and `%TEMP%\OVMF_VARS_secureboot.fd` so the next QEMU launch starts with fresh NVRAM.

**Linux equivalent:** `rm /tmp/OVMF_VARS_4M_{kvm,tcg}.fd` between runs (for the forced KVM / TCG launchers) and `rm build/OVMF_VARS_4M.fd` (for the default `run-qemu.sh`). No shell script ships today.

---

## Bare-Metal Bring-Up

Bare metal is the shipping target; VM launchers filter regressions but do not replace it.

1. **Build the image.** `bash scripts/build.sh clean` -> `build/system-disk.img` (GPT: EFI + BlackBox + IXFS). Image is self-contained: UEFI bootloader, kernel, user-mode shell, resources, configuration.

2. **Disk-image handoff.** Use the deploy helpers when possible -- they guard against writing to a fixed drive by accident:
   - Linux: [`sudo bash scripts/deploy/write-usb.sh`](../../scripts/deploy/write-usb.sh) (lists removable drives only, double-confirm); raw fallback `dd if=build/system-disk.img of=/dev/sdX bs=4M status=progress conv=fsync`.
   - Windows: [`scripts/deploy/write-usb.ps1`](../../scripts/deploy/write-usb.ps1) and [`write-usb.bat`](../../scripts/deploy/write-usb.bat); raw fallback Win32DiskImager or Rufus.
   - Target device: USB stick, SATA SSD, or NVMe drive that the target BIOS will boot.

3. **Serial capture at 115200 8N1** (matches `src/kernel/drivers/serial.c`).
   - RS-232 header or USB-serial adapter -> host capture: `screen /dev/ttyUSB0 115200`, `picocom -b 115200 /dev/ttyUSB0`, or PuTTY serial mode.
   - No serial port on the target? Use the on-disk BlackBox partition (step 4) for post-boot diagnostics; framebuffer capture via HDMI/USB capture card is a last resort.

4. **Post-boot artifact locations** (BlackBox FAT32 partition on the system disk, mountable from any OS that speaks FAT32):
   - `X:\Logs\Serial\` -- klog output.
   - `X:\Boot\` -- per-session boot logs.
   - `X:\Crash\` -- crash recovery + WER-style staging.
   - `X:\Perf\`, `X:\Diag\` -- performance and diagnostic dumps.
   - Full layout and rotation policy: [`todo/01-boot-platform/TODO-24-blackbox-service-partition.md`](../../todo/01-boot-platform/TODO-24-blackbox-service-partition.md).

5. **Offline extraction.**
   - [`bash scripts/tools/read-blackbox.sh`](../../scripts/tools/read-blackbox.sh) -- mount the BlackBox partition from `build/system-disk.img` or an imaged device.
   - [`sudo bash scripts/deploy/read-usb-log.sh [/dev/sdX]`](../../scripts/deploy/read-usb-log.sh) -- after booting on real hardware, mounts the Logs partition read-only and copies `debug.log` + hardware info off a USB target.

6. **Debugger.** No in-kernel gdb stub on bare metal today. Debug via serial log + `llvm-addr2line-19 -e build/kernel.exe -f <RIP>` against panic dumps. JTAG / DCI bring-up is future work, not in scope for this doc.

7. **Bare-metal-only bug classes** (VMs hide these): MMIO cache attributes, per-CPU MSRs reset by CR3 reloads, LAPIC TPR writes, SMEP/SMAP state, MTRR/PAT interactions, DMA cache snooping, USB handover timing, firmware variable persistence. See CLAUDE.md "Bare Metal Gotchas." If a regression only manifests on bare metal, invoke `/debug-session` with the bare-metal serial capture as evidence.

---

## Known Drift

Surfaced here so new contributors can find them instead of tripping over them:

- **`run-qemu-kvm.bat` forces WHPX**, not KVM. Name is historical and stays to avoid breaking muscle memory. `run-qemu-kvm.sh` does use KVM.
- **`run-secureboot.bat` lives under `scripts/debug/`**, not `scripts/machines/`. Logically a machine profile; consolidation under `scripts/machines/` is follow-up work.
- **`scripts/debug/kernel/run-*-tests.bat` are not machine profiles** -- they are category-specific test runners that wrap `run-qemu.ps1 -TestOnly -TestSuite <cat>`. Owned by the [Wrapper Contract section of the developer tooling roadmap](../../todo/00-infrastructure/TODO-01-developer-tooling-stack.md#3-build-test-lint-and-run-wrapper-contract).
- **Filesystem and storage harnesses are Windows-first.** No `.sh` equivalents today; Linux contributors invoke `run-qemu.sh` with extra `-drive ...` manually, or run the Windows-side scripts from WSL2.

---

## Scope Boundary

| Concern                                                  | Owner                                                                                                                        |
| -------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------- |
| Release-validation VM sweep (QEMU + VirtualBox matrices) | [Release QA roadmap -- QEMU + VirtualBox sections](../../todo/15-installer-release/TODO-04-release-qa.md)                        |
| Installer / provisioning VM templates                    | [Unattended Install roadmap -- VM Provisioning Templates section](../../todo/15-installer-release/TODO-02-unattended-install.md) |
| Hyper-V certification flows                              | [Release QA roadmap -- Hyper-V Certification section](../../todo/15-installer-release/TODO-04-release-qa.md) -- deliberately not in the developer matrix |
| Real-hardware certification checklist                    | [Release QA roadmap -- Real Hardware Test Checklist section](../../todo/15-installer-release/TODO-04-release-qa.md)              |
| Performance benchmarks                                   | [Release QA roadmap -- Performance Benchmarks section](../../todo/15-installer-release/TODO-04-release-qa.md)                    |
| GitHub Actions workflow matrix                           | [Developer Tooling roadmap -- GitHub Actions section](../../todo/00-infrastructure/TODO-01-developer-tooling-stack.md#6-github-actions-and-artifact-policy-alignment) |

---

## Updating This Doc

When adding a new launcher:

1. Put the script under the correct subdirectory: `scripts/machines/` for default-path variants, `scripts/machines/fs/` or `scripts/machines/storage/` for scenario-specific variants, `scripts/debug/` only when the primary purpose is debugging a narrow failure mode.
2. Add a subsection above with host, accelerator, use, artifacts, debugger entry point, and any known limitations.
3. If the new launcher drops an existing entry in **Known Drift**, delete that entry.
4. Cross-check that `docs/infrastructure/development-tooling.md` still points here and does not inline a parallel launcher list.
