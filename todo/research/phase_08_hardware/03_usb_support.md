# 25 — USB Support

## Overview

USB host controller drivers + device class drivers for keyboards, mice, and mass storage (flash drives).

---

## USB Stack Layers

```
USB Device Drivers (HID, Mass Storage, Audio)
        ↓
USB Core (enumeration, descriptors, transfers)
        ↓
Host Controller Driver (xHCI / EHCI / UHCI)
        ↓
PCI Bus (controller is a PCI device)
```

## Host Controllers

| Controller | USB version | QEMU flag | Complexity |
|-----------|------------|-----------|-----------|
| **UHCI** | USB 1.1 | `-device piix3-usb-uhci` | ~500 lines |
| **EHCI** | USB 2.0 | `-device usb-ehci` | ~800 lines |
| **xHCI** | USB 3.0+ | `-device qemu-xhci` | ~1200 lines |

## USB Device Classes

| Class | Devices | Priority | Lines |
|-------|---------|----------|-------|
| **HID** | Keyboard, mouse | P0 | ~300 |
| **Mass Storage** | Flash drives | P1 | ~500 |
| **Audio** | USB headsets | P3 | ~800 |

## QEMU testing
```bash
# USB keyboard + mouse
qemu ... -device qemu-xhci -device usb-kbd -device usb-mouse

# USB drive
qemu ... -device qemu-xhci -drive file=usb.img,if=none,id=usb0 \
         -device usb-storage,drive=usb0
```

## Files: `src/kernel/drivers/usb/xhci.c`, `src/kernel/drivers/usb/usb_hid.c`, `src/kernel/drivers/usb/usb_msc.c`
## Implementation: 4-8 weeks (complex)
