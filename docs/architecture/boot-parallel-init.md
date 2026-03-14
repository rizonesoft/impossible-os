# Parallel Boot Initialization

## Dependency Graph

```
pmm → vmm → heap → ata/ahci/virtio_blk → vfs_init
                                                ↓
                    gdt → idt → pic → pit → fb → splash
                                         ↓
                              sti() ─────┬──────────────────────────┐
                                         │                           │
                              dhcp_discover()            partition_scan_all()
                             (fire-and-forget)           partition_mount_filesystems()
                                         │                           │
                              [DHCP round-trip ←──── overlaps ──────┘
                               via network IRQ]
                                         ↓
                              ttf_mgr_init() → icon_store_init() → cursor_init()
                                                                        ↓
                                                         wm_init() → desktop_init()
                                                                        ↓
                                                              boot_splash_finish()
                                                                        ↓
                                                              compositor loop
```

## Implemented Optimization: DHCP Fire-and-Forget

`dhcp_discover()` sends a single UDP broadcast and returns in <1ms. The
OFFER/REQUEST/ACK exchange is entirely interrupt-driven — `dhcp_handle()` is
called from the network IRQ handler when packets arrive.

**Before:** DHCP was called after `partition_mount_filesystems()` — the ~300ms
network round-trip latency ran sequentially after ~500ms of disk scanning.

**After:** DHCP is called immediately after `sti()` — the network round-trip
overlaps with partition scanning and filesystem mounting. By the time
`partition_mount_filesystems()` completes, `net_cfg.configured` is typically
already `1`.

**Estimated saving:** ~200–300ms (DHCP round-trip latency removed from the
critical path).

## Profiled Boot Stages (via serial timestamps)

Enable QEMU serial output and look for `[xx.xxx]` timestamps:

```
[  0.000] --- Phase: PCI & network hardware ---
[  0.180] --- Phase: network (DHCP, async fire-and-forget) ---
[  0.181] --- Phase: partition & filesystem mount ---
[  0.480] [OK] net: DHCP ACK received   ← overlapped, already done
[  0.490] --- Phase: desktop & WM ---
...
[  1.190] [OK] boot: Boot complete in 1.190s
```

## Stages That Cannot Be Parallelized Without VFS Locking

These stages all call into the VFS/FAT32 layer which has **no mutex protection**.
Running them concurrently would cause data corruption:

| Stage | Blocks on | Can parallelize when |
|---|---|---|
| `ttf_mgr_init()` | VFS reads (fonts) | VFS adds reader lock |
| `icon_store_init()` | VFS reads (icons) | VFS adds reader lock |
| `cursor_init()` | VFS reads (cursors) | VFS adds reader lock |
| `desktop_init()` | VFS reads (wallpaper) | VFS adds reader lock |

**Next step to unlock fuller parallelism:** Add a simple `vfs_lock()`/`vfs_unlock()`
reader-writer mutex to `vfs.c`. Once that exists, icon_store and cursor can each
run as a `task_create()` background task, saving another ~100–200ms.

## Future: `icon_store_init()` ∥ `cursor_init()`

When VFS is thread-safe, add this pattern to `main.c`:

```c
/* Enable preemptive scheduler (already safe here — GDT/IDT/PIT ready) */
scheduler_enable();

/* Spawn icon store load on background task */
volatile int icon_done = 0;
task_create(icon_store_init_task, "IconStoreLoad");

/* Cursor load on main thread in parallel */
cursor_init();

/* Wait for icon store */
while (!icon_done)
    __asm__ volatile ("hlt");

scheduler_disable();
```

Estimated additional saving: ~100–200ms.
