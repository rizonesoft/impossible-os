# 82 — Hardware Abstraction Layer (HAL)

## Overview
Abstraction layer between hardware and OS. Drivers implement a standard interface; OS doesn't care about specific hardware.

## Abstractions
| Abstraction | Implementations |
|------------|----------------|
| `net_driver_t` | RTL8139, virtio-net, e1000 |
| `blk_driver_t` | virtio-blk, AHCI, NVMe, IDE |
| `audio_driver_t` | AC97, Intel HDA, SB16 |
| `input_driver_t` | PS/2, USB HID, virtio-input |
| `gpu_driver_t` | VGA/FB, virtio-gpu, real GPU |

```c
/* hal.h — generic block device interface */
struct blk_ops {
    int (*read)(void *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write)(void *dev, uint64_t lba, uint32_t count, const void *buf);
    uint64_t (*capacity)(void *dev);  /* total sectors */
};
```

## Auto-detect: PCI scan → match vendor:device → load appropriate driver
## Files: `include/hal.h` (~100 lines), `src/kernel/hal.c` (~200 lines) | 1 week
