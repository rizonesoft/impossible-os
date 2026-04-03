# Running Impossible OS in QEMU

QEMU is the fastest way to run Impossible OS. It supports UEFI boot via OVMF
firmware and AHCI storage out of the box.

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

```bash
qemu-system-x86_64 \
    -m 256M \
    -drive if=none,id=disk,file=system-disk.img,format=raw \
    -device ahci,id=ahci \
    -device ide-hd,drive=disk,bus=ahci.0 \
    -bios /usr/share/OVMF/OVMF_CODE.fd \
    -serial file:serial.log
```

---

## Recommended Settings

| Setting | Value | Why |
|---------|-------|-----|
| RAM | `-m 256M` | Minimum. 512M recommended for desktop |
| Storage | AHCI (`-device ahci`) | Impossible OS uses AHCI DMA -- no IDE/PIO |
| Firmware | OVMF (`-bios`) | UEFI required -- legacy BIOS will NOT boot |
| Display | Default SDL or `-display gtk` | VGA framebuffer via GOP |
| Serial | `-serial file:serial.log` | Captures boot log for debugging |
| Network | `-netdev user,id=net0 -device rtl8139,netdev=net0` | RTL8139 NIC driver |

---

## Common OVMF Paths

| Distro | OVMF Path |
|--------|-----------|
| Ubuntu/Debian | `/usr/share/OVMF/OVMF_CODE.fd` |
| Fedora | `/usr/share/edk2/ovmf/OVMF_CODE.fd` |
| Arch | `/usr/share/edk2/x64/OVMF_CODE.fd` |
| macOS (Homebrew) | `/opt/homebrew/share/qemu/edk2-x86_64-code.fd` |

---

## Full Example (Networking + VirtIO Tablet)

```bash
qemu-system-x86_64 \
    -m 512M \
    -drive if=none,id=disk,file=system-disk.img,format=raw \
    -device ahci,id=ahci \
    -device ide-hd,drive=disk,bus=ahci.0 \
    -bios /usr/share/OVMF/OVMF_CODE.fd \
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
| Black screen | Verify OVMF path exists. Try `-bios` with full path |
| No disk detected | Must use `-device ahci` + `-device ide-hd`. Legacy `-hda` won't work |
| Mouse stuck in corner | Add `-device virtio-tablet-pci` for absolute positioning |
| Kernel panic on boot | Check `serial.log` for the full stack trace |
| "No bootable device" | UEFI requires GPT + EFI System Partition. Verify disk image integrity |
