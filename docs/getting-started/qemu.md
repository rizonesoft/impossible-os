# Running Impossible OS in QEMU

QEMU is the fastest way to run Impossible OS. It supports UEFI boot via OVMF
firmware and AHCI storage out of the box.

> [!IMPORTANT]
> **Running the March 2026 demo (`v26.3.18-alpha.821`)? Use 1 CPU.** In the QEMU command below, change `-smp 2` to `-smp 1`, or set 1 vCPU in virt-manager. Tested on QEMU 10.2 with KVM and with software emulation (TCG). To keep 2 CPUs instead, replace the `-device ahci` and `-device ide-hd` lines with `-device virtio-blk-pci,drive=disk,disable-modern=on`. With 2 or more CPUs and an AHCI (SATA) disk, that build stops at "Timer: sleeping 1 second" ([issue 6](https://github.com/rizonesoft/impossible-os/issues/6)). Later builds boot with several CPUs.

---

## Prerequisites

### Ubuntu / Debian / WSL 2

```bash
sudo apt install qemu-system-x86 ovmf
```

### Fedora

```bash
sudo dnf install qemu-system-x86 edk2-ovmf
```

### Arch Linux

```bash
sudo pacman -S qemu-system-x86 edk2-ovmf
```

### macOS (Homebrew)

```bash
brew install qemu
```

> [!NOTE]
> On macOS, OVMF firmware is bundled with QEMU. No separate install needed.

---

## Quick Start

UEFI firmware comes in two files: a read-only CODE file and a VARS file where the firmware keeps its settings. Give QEMU your own copy of VARS, then pass both as pflash drives (the paths below are Ubuntu and Debian; other distros are in [Common OVMF Paths](#common-ovmf-paths)):

```bash
cp /usr/share/OVMF/OVMF_VARS_4M.fd ./OVMF_VARS.fd

qemu-system-x86_64 \
    -machine q35 \
    -smp 2 \
    -m 2G \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=./OVMF_VARS.fd \
    -drive if=none,id=disk,file=system-disk.img,format=raw \
    -device ahci,id=ahci \
    -device ide-hd,drive=disk,bus=ahci.0 \
    -serial file:serial.log
```

`-smp` is the number of CPUs: use `-smp 1` for the March 2026 demo. On Linux, add `-accel kvm -cpu host` to run at full speed.

Older guides use `-bios /usr/share/OVMF/OVMF_CODE.fd`. Current Ubuntu releases no longer ship that file, so QEMU reports that it cannot find the firmware. Use the `_4M` files above.

---

## virt-manager (libvirt)

1. Create a new VM, choose **Import existing disk image** and select `system-disk.img`.
2. For the operating system, pick **Generic** or **Generic Linux**, not Windows 11. The Windows 11 template adds Hyper-V enlightenments, a TPM and Secure Boot, and disables HPET.
3. Tick **Customize configuration before install**. Under **Overview**, set Chipset to **Q35** and Firmware to the UEFI entry **without** `secboot` in its name.
4. Under the disk, set the bus to **SATA** (AHCI).
5. Under **CPUs**, set 1 vCPU for the March 2026 demo. Under **Memory**, set 2048 MiB.
6. Add a **Serial** device with **Device type: Output to a file**, so the boot log is saved if anything goes wrong.

## Recommended Settings

| Setting | Value | Why |
|---------|-------|-----|
| RAM | `-m 2G` | Current builds halt at 1 GiB or less ([open bug](../../todo/00-infrastructure/TODO-08-automation-hardening.md#72-the-direct-map-check-passes-on-machines-with-1-gib-or-less)) |
| CPUs | `-smp 2` | Use `-smp 1` for the March 2026 demo |
| Storage | AHCI (`-device ahci`) | Impossible OS uses AHCI DMA -- no IDE/PIO |
| Firmware | OVMF CODE + VARS (`-drive if=pflash`) | UEFI required -- legacy BIOS will NOT boot |
| Display | Default SDL or `-display gtk` | VGA framebuffer via GOP |
| Serial | `-serial file:serial.log` | Captures boot log for debugging |
| Network | `-netdev user,id=net0 -device rtl8139,netdev=net0` | RTL8139 NIC driver |

---

## Common OVMF Paths

| Distro | CODE file | VARS file to copy |
|--------|-----------|-------------------|
| Ubuntu 22.04+ / Debian 12+ | `/usr/share/OVMF/OVMF_CODE_4M.fd` | `/usr/share/OVMF/OVMF_VARS_4M.fd` |
| Fedora | `/usr/share/edk2/ovmf/OVMF_CODE.fd` | `/usr/share/edk2/ovmf/OVMF_VARS.fd` |
| Arch | `/usr/share/edk2/x64/OVMF_CODE.4m.fd` | `/usr/share/edk2/x64/OVMF_VARS.4m.fd` |
| macOS (Homebrew) | `$(brew --prefix)/share/qemu/edk2-x86_64-code.fd` | `$(brew --prefix)/share/qemu/edk2-i386-vars.fd` |

File names change between distro releases. If a path is missing, list what your package installed: `ls /usr/share/OVMF /usr/share/edk2/*/`.

---

## Full Example (Networking + VirtIO Tablet)

```bash
qemu-system-x86_64 \
    -machine q35 \
    -smp 2 \
    -m 2G \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=./OVMF_VARS.fd \
    -drive if=none,id=disk,file=system-disk.img,format=raw \
    -device ahci,id=ahci \
    -device ide-hd,drive=disk,bus=ahci.0 \
    -netdev user,id=net0 \
    -device rtl8139,netdev=net0 \
    -device virtio-tablet-pci \
    -serial file:serial.log \
    -display gtk
```

---

## Troubleshooting

| Issue | Solution |
|-------|----------|
| "Could not open" the firmware file | The path is wrong for your distro; see [Common OVMF Paths](#common-ovmf-paths) |
| Black screen | Verify both OVMF files exist and that the VARS copy is writable |
| Boot stops at "Timer test (1s sleep)" | On the March 2026 demo: use 1 CPU (`-smp 1`). On a later build: attach `serial.log` to an issue |
| "HHDM: identity vs direct-map readback mismatch" | The VM has 1 GiB of RAM or less. Use `-m 2G` |
| No disk detected | Must use `-device ahci` + `-device ide-hd`. Legacy `-hda` won't work |
| Mouse stuck in corner | Add `-device virtio-tablet-pci` for absolute positioning |
| Kernel panic on boot | Check `serial.log` for the full stack trace |
| "No bootable device" | UEFI requires GPT + EFI System Partition. Verify disk image integrity |
