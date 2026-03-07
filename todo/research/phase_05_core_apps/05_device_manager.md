# 41 — Device Manager

## Overview

Lists all detected hardware, drivers loaded, and device status. Like Windows Device Manager.

---

## Layout

```
┌──────────────────────────────────────────────┐
│  Device Manager                         ─ □ ×│
├──────────────────────────────────────────────┤
│  ▼ 💻 Impossible OS                          │
│    ▼ Display adapters                        │
│      📺 VGA Compatible (Multiboot2 FB)       │
│    ▼ Network adapters                        │
│      🌐 Realtek RTL8139 (PCI)                │
│    ▼ Storage controllers                     │
│      💾 VirtIO Block Device                  │
│    ▼ Input devices                           │
│      ⌨️ PS/2 Keyboard (IRQ 1)               │
│      🖱 VirtIO Tablet (PCI)                  │
│    ▼ System devices                          │
│      ⏱ PIT Timer (IRQ 0)                    │
│      🔌 PCI Bus                              │
│      📍 ACPI                                 │
│    ▼ Sound, video and game controllers       │
│      🔊 AC97 Audio (PCI)                     │
└──────────────────────────────────────────────┘
```

## Data source: PCI enumeration + registered drivers
```c
struct device_info {
    char     name[64];
    char     driver[32];     /* loaded driver name, or "No driver" */
    char     category[32];   /* "Network", "Storage", etc. */
    uint16_t vendor_id;      /* PCI vendor */
    uint16_t device_id;      /* PCI device */
    uint8_t  bus, slot, func;
    uint8_t  irq;
    uint8_t  status;         /* OK, error, no driver */
};
```

## Files: `src/apps/devmgr/devmgr.c` (~400 lines)
## Implementation: 3-5 days
