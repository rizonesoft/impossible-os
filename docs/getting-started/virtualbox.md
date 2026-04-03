# Running Impossible OS in VirtualBox

VirtualBox provides a user-friendly GUI for running Impossible OS. It requires
UEFI mode and a VDI disk image (included in releases).

---

## Prerequisites

- **VirtualBox 7.0+** -- [Download](https://www.virtualbox.org/wiki/Downloads)
- **VirtualBox Extension Pack** (recommended) -- USB 2.0/3.0 support

---

## Option A: Using the Pre-built VDI (Easiest)

Each release includes a `.vdi` file ready for VirtualBox:

1. Download `system-disk.vdi` from the
   [latest release](https://github.com/rizonesoft/impossible-os/releases)
2. Follow **Create the VM** below, and select this VDI as the hard disk

---

## Option B: Convert RAW Image to VDI

If you only have the `.img` file:

```bash
VBoxManage convertfromraw system-disk.img system-disk.vdi --format VDI
```

> [!NOTE]
> On Windows, use the full path: `"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"`

---

## Create the VM

### Via GUI

1. **New** → Name: `Impossible OS`, Type: `Other`, Version: `Other/Unknown (64-bit)`
2. **Memory:** 256 MB minimum (512 MB recommended)
3. **Hard Disk:** "Use an existing virtual hard disk file" → select `system-disk.vdi`
4. **Settings → System:**
   - ✅ **Enable EFI** (required -- Impossible OS does not boot in legacy BIOS)
   - Uncheck "Floppy" from boot order
5. **Settings → Storage:**
   - Controller should be **AHCI** (default for SATA)
6. **Start** the VM

### Via Command Line

```bash
# Create VM
VBoxManage createvm --name "Impossible OS" --ostype Other_64 --register

# Configure
VBoxManage modifyvm "Impossible OS" \
    --memory 512 \
    --firmware efi \
    --graphicscontroller vmsvga \
    --vram 64 \
    --nic1 nat \
    --nictype1 82540EM

# Attach storage
VBoxManage storagectl "Impossible OS" \
    --name "SATA" \
    --add sata \
    --controller IntelAhci

VBoxManage storageattach "Impossible OS" \
    --storagectl "SATA" \
    --port 0 \
    --device 0 \
    --type hdd \
    --medium system-disk.vdi

# Start
VBoxManage startvm "Impossible OS"
```

---

## Recommended Settings

| Setting | Value | Why |
|---------|-------|-----|
| Firmware | **EFI** | UEFI required -- legacy BIOS will NOT boot |
| RAM | 256–512 MB | 256 MB minimum for desktop |
| VRAM | 64 MB | Recommended for 1280×720 framebuffer |
| Storage | AHCI/SATA | Impossible OS uses AHCI DMA drivers |
| Network | NAT + Intel PRO/1000 | Default works; RTL8139 also supported |
| Audio | Disabled | Not yet implemented |

---

## Troubleshooting

| Issue | Solution |
|-------|----------|
| "No bootable medium found" | Enable EFI in Settings → System → Enable EFI |
| Black screen after UEFI logo | Increase VRAM to 64 MB or higher |
| VM crashes on start | Ensure "Other/Unknown (64-bit)" is the OS type |
| Mouse not working | VirtualBox mouse integration may conflict; try disabling it |
| Slow performance | Enable VT-x/AMD-V in BIOS and in VirtualBox settings |
| Cannot convert `.img` to `.vdi` | Ensure VBoxManage is in your PATH, or use full path |

---

## Accessing Serial Output

To capture kernel serial output in VirtualBox:

1. **Settings → Serial Ports → Port 1**
2. ✅ Enable Serial Port
3. Port Mode: **Raw File**
4. Path: `/path/to/serial.log` (or `C:\Users\You\serial.log` on Windows)
5. Port Number: **COM1** (IRQ 4, I/O 0x3F8)
