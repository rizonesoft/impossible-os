<!-- docs: covers=todo/01-boot-platform/TODO-16-nvme-storage.md sources=src/kernel/drivers/nvme.c,include/kernel/drivers/nvme.h,src/kernel/main/blkdev_adapters.c,src/kernel/main/boot_storage.c,src/kernel/drivers/blkdev.c,src/kernel/acpi.c,src/kernel/test/test_nvme.c reviewed=2026-09-28 order=16 -->
# NVMe Storage Driver

## What is it?

This is the boot-critical driver that lets Impossible OS use an NVMe SSD as a block device, which matters because most laptops built since 2018 have NVMe as their only storage. It discovers NVMe controllers on PCI, brings up one Admin Queue and one I/O queue pair per controller, and registers each usable namespace with the block-device layer so partitions can be scanned and a filesystem mounted.

The driver is built into the kernel image today, rather than loaded as a separate boot-start driver the way Windows loads `stornvme.sys`; that split waits on the kernel module loader.

## How does it work?

`nvme_init()` walks PCI for class 0x01, subclass 0x08, prog-if 0x02 (NVM Express), reads the 64-bit BAR0 and maps a 16 KiB register window uncacheable with `vmm_map_mmio_uc()`. It rejects a controller whose doorbell stride would put a doorbell it writes past that window, and one whose `CAP.MPSMIN` is not 0, because the driver only programs 4 KiB pages. The controller is then disabled (`CC.EN=0`, wait for `CSTS.RDY=0`) and enabled (`CC.EN=1`, wait for `CSTS.RDY=1`), checking `CSTS.CFS` for a fatal status at each wait.

Next, Identify Controller reads the model and serial strings and Identify Namespace (NSID 1) reads the LBA count and formatted LBA size. Only 512-byte and 4096-byte sectors are accepted; any other size leaves the namespace unusable, so it is never registered rather than risking a bad shift on an out-of-range exponent. For a usable namespace the driver creates one I/O completion queue and one I/O submission queue (both queue ID 1), caps the depth to the controller's `MQES+1`, and reads sector 0 to prove the path works.

Reads and writes (`nvme_read_sectors()`, `nvme_write_sectors()`) bounds-check the LBA range against the namespace size with overflow-safe arithmetic, then submit one command per 4 KiB DMA page. Completion is polled on the completion-queue phase bit. A command that never completes poisons the queue, so a late completion cannot be mistaken for the next command's. Submission is serialized by an atomic compare-and-swap gate (`io_busy`) rather than a spinlock, because the poll loop sleeps with `hlt` and a spinlock would hold interrupts off across that sleep.

Two lifecycle paths make durability real. `nvme_flush()` issues an NVM Flush and is the block device's `flush` callback, so `blkdev_sync()` actually commits the drive's volatile write cache. `nvme_shutdown()` refuses new I/O, drains any in-flight command, sets `CC.SHN` to normal shutdown and polls `CSTS.SHST`, bounded by the controller's advertised timeout. Every registered block device is flushed and shut down by `acpi_storage_quiesce()`, which both power-off and reboot run before disabling interrupts.

`blkdev_register_all()` registers each controller with an active I/O queue and non-zero geometry as `nvme0` to `nvme3`. `boot_phase2()` runs `nvme_init()` either in sequence or as an async storage task; finding no controller is a normal outcome, not a degraded boot, since many machines are SATA-only.

## What are its interfaces?

| Interface | Purpose |
| --------- | ------- |
| `nvme_init()` | PCI scan and per-controller bringup; returns the controller count ([`nvme.c`](../../src/kernel/drivers/nvme.c)) |
| `nvme_controller_count()` / `nvme_get_controller(idx)` | Query the discovered-controller table ([`nvme.c`](../../src/kernel/drivers/nvme.c)) |
| `nvme_read_sectors()` / `nvme_write_sectors()` | Bounds-checked, page-chunked sector I/O over the polled I/O queue ([`nvme.c`](../../src/kernel/drivers/nvme.c)) |
| `nvme_flush(ctrl_idx)` | NVM Flush; the block device's `flush` callback ([`nvme.c`](../../src/kernel/drivers/nvme.c)) |
| `nvme_shutdown(ctrl_idx)` / `nvme_shutdown_all()` | `CC.SHN` normal-shutdown handshake for one or all controllers ([`nvme.c`](../../src/kernel/drivers/nvme.c)) |
| `struct nvme_controller` | Per-controller state: register window, CAP fields, queues, namespace geometry, `io_busy` and shutdown gates ([`nvme.h`](../../include/kernel/drivers/nvme.h)) |
| Block-device registration | The NVMe loop in `blkdev_register_all()` ([`blkdev_adapters.c`](../../src/kernel/main/blkdev_adapters.c)) |
| Boot wiring | The sequential and async `nvme_init()` calls in `boot_phase2()` ([`boot_storage.c`](../../src/kernel/main/boot_storage.c)) |
| Power-off and reboot quiesce | `acpi_storage_quiesce()` flushes and shuts down every block device ([`acpi.c`](../../src/kernel/acpi.c)) |

## How do I use it?

```bash
make run-nvme                         # QEMU with an NVMe controller and a FAT32 test drive
make run-nvme-ci                      # headless variant, serial captured to build/serial.log
bash scripts/test.sh SUITE=storage    # or: make test-storage
```

On a boot with an NVMe controller, serial (subsystem `nvme`) shows the controller version line, the Identify summary with model and capacity, `I/O Queue created (QID=1, depth=N...`, and `sector 0 read OK`. Without a controller it shows `no controller found` and boot continues. Once block devices are registered, the `blk` subsystem lists each one, for example `nvme0: N MiB (N sectors, 512 B/sec)`. A clean power-off or reboot logs `shutdown complete` when the controller reports shutdown finished.

`src/kernel/test/test_nvme.c` registers a read-only test (`TEST_CAT_STORAGE`) that checks the controller count and, when a controller is present, that the namespace sector size is usable. The read, write, flush and shutdown paths need real NVMe hardware and are validated with `make run-nvme` and on bare metal. Use QEMU TCG rather than WHPX for NVMe testing: WHPX's emulated NVMe I/O is unreliable.

## What is not implemented yet?

Everything below is owned by one backlog section, [Advanced NVMe parity backlog](../../todo/01-boot-platform/TODO-16-nvme-storage.md#5-advanced-nvme-parity-backlog-d04-t08-owned), which hands the work to the [core driver enhancements roadmap](../../todo/04-drivers-hardware/TODO-08-core-driver-enhancements.md).

- Interrupt-driven (MSI-X or MSI) completion: every command is polled.
- More than one I/O queue pair per controller (a per-CPU queue model like Linux `blk-mq`).
- SMART and health log pages, and surfacing the Critical Warning byte.
- Namespaces other than NSID 1.
- Dataset Management (TRIM and discard) wired to the block layer.
- Autonomous Power State Transitions for laptop idle power.
- Multi-page PRP-list transfers; each command moves one 4 KiB page, and each call allocates its DMA page rather than reusing a bounce buffer.
- NVMe over Fabrics, out of scope until networking and RDMA exist.

## How does it compare with Windows 11 and Linux?

Discovery, boot-time mount, write-cache durability through Flush and the clean-shutdown handshake are at parity with `stornvme.sys` and Linux `nvme.ko`. The clearest gap is the I/O path: Windows and Linux drive NVMe with many hardware queues and MSI-X interrupts, while Impossible OS uses one polled queue pair per controller. One small edge is that the drive's Identify model and serial appear in the boot log itself. SMART health, wear and thermal state are not shown at boot here, and are tracked in the backlog above rather than claimed.

## See also

- [NVMe Storage Driver roadmap](../../todo/01-boot-platform/TODO-16-nvme-storage.md)
- [Core driver enhancements roadmap](../../todo/04-drivers-hardware/TODO-08-core-driver-enhancements.md)
- [Boot Device Discovery](boot-device-discovery.md)
- [Bare Metal Gotchas](../infrastructure/bare-metal-gotchas.md)
