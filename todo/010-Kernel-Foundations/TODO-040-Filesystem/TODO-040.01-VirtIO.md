# 040.01-VirtIO — VirtIO Block Device Driver

> **Goal:** Bring the existing VirtIO block driver from basic MMIO polling read/write up to
> production-grade quality. Migrate to modern PCI transport with capability discovery,
> implement MSI-X interrupts, comprehensive feature negotiation (flush, topology, discard,
> write-zeroes, multi-queue), async I/O, error recovery, and device identification —
> all per the VirtIO 1.2 specification (OASIS, July 2022).
> The current driver (`src/kernel/drivers/virtio_blk.c`, ~350 lines) handles MMIO register
> access, split virtqueue setup, 3-descriptor chain read/write, and `VIRTIO_F_VERSION_1`
> negotiation — all via polling with legacy PIC interrupts.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL virtqueue buffers (descriptor tables, available rings, used rings). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). See `rules.md` Known Gotchas.

> [!IMPORTANT]
> **Spec Reference:** All section numbers, register offsets, and bit definitions reference the
> [VirtIO 1.2 Specification](file:///home/derickpayne/impossible-os/specs/virtio-1.2.md)
> (OASIS, 2022). The block-device-focused summary is in the repo at `specs/virtio-1.2.md`.

---

## 1. Modern PCI Transport

### 1.1 PCI Capability Discovery

**Prompt:** The current driver uses hardcoded MMIO register offsets, which only works with the MMIO transport (not PCI). Modern VirtIO 1.0+ PCI devices expose configuration via VirtIO Structure PCI Capabilities (vendor-specific cap ID `0x09`). Walk the PCI capability list starting from config offset `0x34`, following `cap_next` links. For each capability with `cap_vndr == 0x09`, record the `cfg_type`, `bar`, `offset`, and `length`. Map the referenced BAR into kernel virtual memory. The driver needs pointers to: `VIRTIO_PCI_CAP_COMMON_CFG` (type 1), `VIRTIO_PCI_CAP_NOTIFY_CFG` (type 2), `VIRTIO_PCI_CAP_ISR_CFG` (type 3), and `VIRTIO_PCI_CAP_DEVICE_CFG` (type 4). Also read the `notify_off_multiplier` from the notification capability. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: PCI capability discovery"`. Add notes directly in this TODO section.

- [ ] Detect VirtIO PCI device: Vendor ID `0x1AF4`, Device ID `0x1042` (modern) or `0x1001` (transitional block)
- [ ] For transitional `0x1001`, verify Subsystem Device ID == `0x0002`
- [ ] Enable PCI Bus Mastering (Command register bit 2) and Memory Space (bit 1)
- [ ] Walk PCI capability list from offset `0x34`:
  - [ ] For each cap with `cap_vndr == 0x09`: parse `cfg_type`, `bar`, `offset`, `length`
  - [ ] Store pointer to `COMMON_CFG` (type 1), `NOTIFY_CFG` (type 2), `ISR_CFG` (type 3), `DEVICE_CFG` (type 4)
- [ ] Map the BAR(s) into kernel address space (identity-mapped MMIO)
- [ ] Read `notify_off_multiplier` from the notification capability's extended field
- [ ] Compute per-queue notification address: `BAR_base + cap.offset + (queue_notify_off * notify_off_multiplier)`
- [ ] Fallback: if no PCI caps found, use legacy MMIO transport (current code path)
- [ ] Log: `[VirtIO] PCI caps: common_cfg @ BAR%d+0x%x, notify @ BAR%d+0x%x, device_cfg @ BAR%d+0x%x`
- [ ] Commit: `"virtio-blk: PCI capability discovery"`

### 1.2 Modern Initialization Sequence

**Prompt:** The current driver performs a simplified init. Implement the full VirtIO 1.0+ 7-step initialization using the common_cfg MMIO structure: (1) reset by writing 0 to `device_status`, (2) set `ACKNOWLEDGE`, (3) set `DRIVER`, (4) read/write feature pages via `device_feature_select`/`driver_feature_select`, (5) set `FEATURES_OK` and re-read to confirm, (6) setup virtqueues via `queue_select`/`queue_size`/`queue_desc`/`queue_driver`/`queue_device`/`queue_enable`, (7) set `DRIVER_OK`. Use the `config_generation` counter for atomic config reads. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: modern init sequence"`. Add notes directly in this TODO section.

- [ ] Step 1: Write `0` to `device_status` → full reset, wait for readback `0`
- [ ] Step 2: Set `ACKNOWLEDGE` (bit 0) in `device_status`
- [ ] Step 3: Set `DRIVER` (bit 1) in `device_status`
- [ ] Step 4: Feature negotiation:
  - [ ] Write `device_feature_select = 0`, read `device_feature` (bits 0–31)
  - [ ] Write `device_feature_select = 1`, read `device_feature` (bits 32–63)
  - [ ] Accept features: write `driver_feature_select = 0/1`, write `driver_feature`
  - [ ] Must accept `VIRTIO_F_VERSION_1` (bit 32) — reject device if not offered
- [ ] Step 5: Set `FEATURES_OK` (bit 3), re-read `device_status` — if `FEATURES_OK` cleared, abort
- [ ] Step 6: For queue 0 (request queue):
  - [ ] Write `queue_select = 0`
  - [ ] Read `queue_size` (max descriptors)
  - [ ] Allocate descriptor table, available ring, used ring via `pmm_alloc_contiguous()`
  - [ ] Write physical addresses to `queue_desc`, `queue_driver`, `queue_device` (64-bit)
  - [ ] Write `queue_enable = 1`
- [ ] Step 7: Set `DRIVER_OK` (bit 2) — device is live
- [ ] Implement `config_generation` read loop for atomic device config access
- [ ] Check for `DEVICE_NEEDS_RESET` (bit 6) after init — abort if set
- [ ] Commit: `"virtio-blk: modern init sequence"`

---

## 2. Feature Negotiation

### 2.1 Block Size & Topology

**Prompt:** The current driver assumes 512-byte sectors and has no awareness of physical block alignment. Negotiate `VIRTIO_BLK_F_BLK_SIZE` (bit 5) to read the logical block size from `blk_size` at device config offset `0x14`. Negotiate `VIRTIO_BLK_F_TOPOLOGY` (bit 7) to read the physical block exponent, alignment offset, and optimal I/O size from config offsets `0x18`–`0x1C`. Also negotiate `VIRTIO_BLK_F_SEG_MAX` (bit 1) and `VIRTIO_BLK_F_SIZE_MAX` (bit 0) to read segment limits. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: block size and topology negotiation"`. Add notes directly in this TODO section.

- [ ] Negotiate `VIRTIO_BLK_F_BLK_SIZE` (bit 5) — read `blk_size` from device config offset `0x14`
- [ ] Negotiate `VIRTIO_BLK_F_TOPOLOGY` (bit 7) — read `physical_block_exp`, `alignment_offset`, `min_io_size`, `opt_io_size`
- [ ] Negotiate `VIRTIO_BLK_F_SIZE_MAX` (bit 0) — read `size_max` (max bytes per segment)
- [ ] Negotiate `VIRTIO_BLK_F_SEG_MAX` (bit 1) — read `seg_max` (max segments per request)
- [ ] Store all values in `virtio_blk_dev` struct for use by I/O path
- [ ] Enforce `seg_max` limit in `virtio_blk_read()`/`virtio_blk_write()` — split large I/Os
- [ ] Default to 512-byte sectors if `F_BLK_SIZE` not offered
- [ ] Log: `[VirtIO] Block: capacity=%llu sectors, blk_size=%u, opt_io=%u`
- [ ] Commit: `"virtio-blk: block size and topology negotiation"`

### 2.2 Flush (Write Barriers)

**Prompt:** The current driver has no cache flush support, meaning filesystem write barriers (e.g., FAT32 metadata commit, IXFS journal) cannot guarantee durability. Negotiate `VIRTIO_BLK_F_FLUSH` (bit 6). When negotiated, implement `virtio_blk_flush()` that sends a `VIRTIO_BLK_T_FLUSH` (type 4) request — a 2-descriptor chain (header + status, no data buffer). The `sector` field is ignored for flush. Also negotiate `VIRTIO_BLK_F_CONFIG_WCE` (bit 9) to read/toggle the writeback cache mode via device config offset `0x20`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: flush and write cache control"`. Add notes directly in this TODO section.

- [ ] Negotiate `VIRTIO_BLK_F_FLUSH` (bit 6) — cache flush supported
- [ ] Implement `virtio_blk_flush()`:
  - [ ] Build 2-descriptor chain: header (type = `VIRTIO_BLK_T_FLUSH`, sector = 0) + status byte
  - [ ] Submit to request queue, wait for completion
  - [ ] Return `VIRTIO_BLK_S_OK` / `VIRTIO_BLK_S_IOERR`
- [ ] Negotiate `VIRTIO_BLK_F_CONFIG_WCE` (bit 9) — writeback cache enable
- [ ] Read/write `writeback` field at device config offset `0x20` (0 = writethrough, 1 = writeback)
- [ ] Expose: `virtio_blk_set_write_cache(bool enable)` public API
- [ ] Wire to block device layer: `blkdev_sync()` → `virtio_blk_flush()`
- [ ] Commit: `"virtio-blk: flush and write cache control"`

### 2.3 Device Identification (GET_ID)

**Prompt:** Implement the `VIRTIO_BLK_T_GET_ID` (type 8) request to retrieve a 20-byte ASCII device serial number from the virtual disk. This is always available (no feature bit required). Use a 3-descriptor chain: header (type = `VIRTIO_BLK_T_GET_ID`) + 20-byte device-writable buffer + status byte. Expose as `virtio_blk_get_id(char *buf, size_t len)`. Register the ID string in the block device registry. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: GET_ID device identification"`. Add notes directly in this TODO section.

- [ ] Implement `virtio_blk_get_id(char *id, size_t len)`:
  - [ ] Build 3-descriptor chain: header (type = `0x08`) + 20-byte writable buffer + status
  - [ ] Null-terminate the returned string
  - [ ] Return status code
- [ ] Call `virtio_blk_get_id()` during init after `DRIVER_OK`
- [ ] Store device ID in `virtio_blk_dev.serial[20]`
- [ ] Register with block device layer: `blkdev_register("virtio0", serial, capacity)`
- [ ] Log: `[VirtIO] Block: ID="%s"`
- [ ] Commit: `"virtio-blk: GET_ID device identification"`

### 2.4 Read-Only Detection

**Prompt:** Check `VIRTIO_BLK_F_RO` (bit 4) during feature negotiation. If the device is read-only, the driver must reject all write requests with an appropriate error code. Store the RO flag in the device struct and check it in `virtio_blk_write()`. Log a warning at init if the device is read-only. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: read-only device detection"`. Add notes directly in this TODO section.

- [ ] Check `VIRTIO_BLK_F_RO` (bit 4) during feature negotiation
- [ ] Store `virtio_blk_dev.read_only = true/false`
- [ ] Guard `virtio_blk_write()`: if `read_only`, return `-EROFS` immediately
- [ ] Guard `virtio_blk_flush()`: if `read_only`, no-op (nothing to flush)
- [ ] Log: `[VirtIO] Block: device is READ-ONLY`
- [ ] Commit: `"virtio-blk: read-only device detection"`

---

## 3. Interrupt-Driven I/O

### 3.1 MSI-X Interrupts

**Prompt:** The current driver uses legacy PIC interrupts, which violates the `rules.md` APIC-only mandate. Modern VirtIO PCI devices support MSI-X for per-queue interrupt vectors. Walk the PCI capability list for MSI-X (cap ID `0x11`). Map the MSI-X Table BAR. Program each table entry with the LAPIC destination address (`0xFEE00000 | (cpu << 12)`) and message data (IDT vector). Assign vectors to virtqueues via `queue_msix_vector` in common_cfg, and a separate vector for config changes via `config_msix_vector`. Enable MSI-X in the PCI Message Control register. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: MSI-X interrupt support"`. Add notes directly in this TODO section.

- [ ] Walk PCI capabilities for MSI-X capability (cap ID `0x11`)
- [ ] Read MSI-X Control: table size, Table BAR + offset, PBA BAR + offset
- [ ] Map MSI-X Table BAR into kernel memory
- [ ] Allocate IDT vectors: 1 for request queue + 1 for config changes
- [ ] Program MSI-X table entry 0: `msg_addr = 0xFEE00000`, `msg_data = idt_vector_queue`
- [ ] Program MSI-X table entry 1: `msg_addr = 0xFEE00000`, `msg_data = idt_vector_config`
- [ ] Write `queue_msix_vector = 0` for request queue in common_cfg
- [ ] Write `config_msix_vector = 1` in common_cfg
- [ ] Enable MSI-X: set bit 15 in PCI MSI-X Message Control register
- [ ] Disable legacy INTx: set PCI Command Register bit 10
- [ ] Register IDT handlers for both vectors
- [ ] Commit: `"virtio-blk: MSI-X interrupt support"`

### 3.2 Async I/O Path

**Prompt:** Replace the current polling loop (`while (used->idx == last_seen_used)`) with interrupt-driven async I/O. Add a per-device completion event. The ISR fires when the device updates the used ring — it reads the completed descriptor heads and wakes waiting threads via `event_set()`. The submission path (`virtio_blk_read`/`virtio_blk_write`) uses `event_wait()` with a configurable timeout (5s default). Retain a polling fallback for pre-scheduler boot (before interrupts are available). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: async interrupt-driven I/O"`. Add notes directly in this TODO section.

- [ ] Add `event_t io_completion` to `virtio_blk_dev` struct
- [ ] Queue ISR: read `used->idx`, process completed entries, call `event_set()`
- [ ] Config ISR: re-read device configuration (capacity, topology changes)
- [ ] Submission path: `event_wait(&io_completion, timeout_ms)` after virtqueue kick
- [ ] Process used ring entries: match `used_elem.id` to pending request, copy status byte
- [ ] Handle timeout: log error, attempt device reset (§5.1)
- [ ] Retain polling path: `if (!scheduler_running) { poll_used_ring(); }`
- [ ] Set `VIRTQ_AVAIL_F_NO_INTERRUPT = 0` in available ring flags (enable interrupts)
- [ ] Commit: `"virtio-blk: async interrupt-driven I/O"`

---

## 4. Discard & Write-Zeroes

### 4.1 Discard (TRIM)

**Prompt:** Negotiate `VIRTIO_BLK_F_DISCARD` (bit 11). Read `max_discard_sectors`, `max_discard_seg`, and `discard_sector_alignment` from device config. Implement `virtio_blk_discard()` using `VIRTIO_BLK_T_DISCARD` (type 0x0B). The data descriptor contains one or more `virtio_blk_discard_write_zeroes` segment structs (16 bytes each: 8-byte sector + 4-byte num_sectors + 4-byte flags). Wire to VFS: `fat32_unlink()` and `ixfs_delete()` call `blkdev_discard()` → `virtio_blk_discard()`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: discard (TRIM) support"`. Add notes directly in this TODO section.

- [ ] Negotiate `VIRTIO_BLK_F_DISCARD` (bit 11)
- [ ] Read config: `max_discard_sectors` (offset `0x24`), `max_discard_seg` (offset `0x28`), `discard_sector_alignment` (offset `0x2C`)
- [ ] Implement `virtio_blk_discard(uint64_t sector, uint32_t num_sectors)`:
  - [ ] Build segment struct: `{ sector, num_sectors, flags = 0 }`
  - [ ] Build 3-descriptor chain: header (type = `0x0B`) + segment data + status
  - [ ] Split requests exceeding `max_discard_sectors`
- [ ] Wire to VFS: `blkdev_discard()` → `virtio_blk_discard()`
- [ ] QEMU test: `-drive ...,discard=unmap -device virtio-blk-pci,...,discard=on`
- [ ] Commit: `"virtio-blk: discard (TRIM) support"`

### 4.2 Write-Zeroes

**Prompt:** Negotiate `VIRTIO_BLK_F_WRITE_ZEROES` (bit 12). Read `max_write_zeroes_sectors`, `max_write_zeroes_seg`, and `write_zeroes_may_unmap` from device config. Implement `virtio_blk_write_zeroes()` using `VIRTIO_BLK_T_WRITE_ZEROES` (type 0x0D). If `write_zeroes_may_unmap` is set and the unmap flag in the segment struct is set, the device may deallocate the zeroed region (thin provisioning). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: write-zeroes support"`. Add notes directly in this TODO section.

- [ ] Negotiate `VIRTIO_BLK_F_WRITE_ZEROES` (bit 12)
- [ ] Read config: `max_write_zeroes_sectors` (offset `0x30`), `max_write_zeroes_seg` (offset `0x34`), `write_zeroes_may_unmap` (offset `0x38`)
- [ ] Implement `virtio_blk_write_zeroes(uint64_t sector, uint32_t num_sectors, bool unmap)`:
  - [ ] Build segment struct: `{ sector, num_sectors, flags = unmap ? 1 : 0 }`
  - [ ] Build 3-descriptor chain: header (type = `0x0D`) + segment data + status
  - [ ] Split requests exceeding `max_write_zeroes_sectors`
- [ ] Wire to filesystem formatting and secure-delete paths
- [ ] Commit: `"virtio-blk: write-zeroes support"`

---

## 5. Error Recovery

### 5.1 Device Reset & Recovery

**Prompt:** Implement comprehensive error handling. Track the status byte of every completed request — on `VIRTIO_BLK_S_IOERR` (0x01), retry the request up to 3 times before reporting failure. On `VIRTIO_BLK_S_UNSUPP` (0x02), log and return "not supported" without retry. Monitor `device_status` bit 6 (`DEVICE_NEEDS_RESET`) — when set, perform a full device reset: write `0` to `device_status`, wait for readback `0`, then re-run the full initialization sequence. Resubmit any pending I/O requests after recovery. Add an I/O timeout (5s) — if no completion arrives, trigger a reset. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: error recovery and device reset"`. Add notes directly in this TODO section.

- [ ] Track status byte for every completed request:
  - [ ] `VIRTIO_BLK_S_OK` (0x00) → success
  - [ ] `VIRTIO_BLK_S_IOERR` (0x01) → retry (up to 3 retries per request)
  - [ ] `VIRTIO_BLK_S_UNSUPP` (0x02) → return error, no retry
- [ ] Monitor `device_status` bit 6 (`DEVICE_NEEDS_RESET`) in ISR and after I/O
- [ ] Implement `virtio_blk_reset()`:
  - [ ] Write `0` to `device_status`
  - [ ] Wait for `device_status` readback `0`
  - [ ] Re-run full 7-step initialization
  - [ ] Resubmit any pending I/O requests from the retry queue
- [ ] I/O timeout: if completion not received within 5 seconds, trigger reset
- [ ] Per-device error counters: I/O errors, resets, timeouts
- [ ] Expose via Registry: `HKLM\HARDWARE\VirtIO\Block0\Errors\*`
- [ ] Log: `[VirtIO] Block: I/O error (status=%d), retry %d/3`
- [ ] Log: `[VirtIO] Block: device needs reset — reinitializing`
- [ ] Commit: `"virtio-blk: error recovery and device reset"`

### 5.2 Individual Queue Reset (VirtIO 1.2+)

**Prompt:** When `VIRTIO_F_RING_RESET` (bit 40) is negotiated, implement per-queue reset without resetting the entire device. Write `1` to `queue_reset` for the target queue, wait for readback `1` (device acknowledged), free old virtqueue memory, reallocate fresh descriptor table / available ring / used ring, write new addresses, then write `0` to `queue_reset` to re-enable. This is less disruptive than a full device reset for transient queue errors. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: individual queue reset"`. Add notes directly in this TODO section.

- [ ] Negotiate `VIRTIO_F_RING_RESET` (bit 40)
- [ ] Implement `virtio_queue_reset(int queue_idx)`:
  - [ ] Write `queue_select = queue_idx`
  - [ ] Write `queue_reset = 1`
  - [ ] Poll until `queue_reset` reads back `1` (device acknowledged)
  - [ ] Free old descriptor table, available ring, used ring
  - [ ] Reallocate via `pmm_alloc_contiguous()`
  - [ ] Write new `queue_desc`, `queue_driver`, `queue_device`
  - [ ] Write `queue_reset = 0` to re-enable
- [ ] Use queue reset as first recovery attempt before full device reset
- [ ] Commit: `"virtio-blk: individual queue reset"`

---

## 6. Multi-Queue Support

### 6.1 Per-CPU Request Queues

**Prompt:** Negotiate `VIRTIO_BLK_F_MQ` (bit 22). Read `num_queues` from device config offset `0x22`. Allocate and initialize `num_queues` independent virtqueues — one per CPU core. Each CPU submits I/O to its local queue (no spinlock required). Assign a unique MSI-X vector per queue. The device processes all queues in parallel. This eliminates virtqueue lock contention and matches modern NVMe's multi-queue model. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: multi-queue support"`. Add notes directly in this TODO section.

- [ ] Negotiate `VIRTIO_BLK_F_MQ` (bit 22)
- [ ] Read `num_queues` from device config offset `0x22`
- [ ] Allocate `num_queues` virtqueue structures (descriptor table + avail + used per queue)
- [ ] Initialize each queue via common_cfg: `queue_select`, `queue_size`, `queue_desc`, etc.
- [ ] Assign unique MSI-X vector per queue: `queue_msix_vector = queue_idx`
- [ ] Register per-queue ISR handlers
- [ ] Route I/O by CPU: `queue = queues[smp_cpu_id() % num_queues]`
- [ ] Each queue has its own `last_seen_used` and completion event
- [ ] Fallback: if `F_MQ` not offered, use single queue 0 (current behavior)
- [ ] QEMU test: `-device virtio-blk-pci,drive=disk0,num-queues=4`
- [ ] Commit: `"virtio-blk: multi-queue support"`

---

## 7. Advanced Virtqueue Features

### 7.1 Indirect Descriptors

**Prompt:** Negotiate `VIRTIO_F_RING_INDIRECT_DESC` (bit 28). When negotiated, a single descriptor in the main ring can point to a buffer containing an array of indirect descriptors. This allows submitting large scatter-gather lists (e.g., for multi-segment I/O) without consuming entries from the main descriptor table. Set `VIRTQ_DESC_F_INDIRECT` flag on the primary descriptor, point `addr` to the indirect table, set `len` to `num_indirect * 16`. The indirect table entries must not themselves set `VIRTQ_DESC_F_INDIRECT`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: indirect descriptors"`. Add notes directly in this TODO section.

- [ ] Negotiate `VIRTIO_F_RING_INDIRECT_DESC` (bit 28)
- [ ] Implement indirect descriptor table allocation (per-request, from a pool)
- [ ] Build indirect table: array of `virtq_desc` entries for header + data segments + status
- [ ] Primary descriptor: `addr = phys(indirect_table)`, `len = count*16`, `flags = INDIRECT`
- [ ] Submit single primary descriptor to available ring (consumes only 1 slot)
- [ ] Use for multi-segment I/O requests exceeding 3 descriptors
- [ ] Fallback: if not negotiated, use direct 3-descriptor chains (current behavior)
- [ ] Commit: `"virtio-blk: indirect descriptors"`

### 7.2 Event Index (Interrupt Coalescing)

**Prompt:** Negotiate `VIRTIO_F_RING_EVENT_IDX` (bit 29). When negotiated, the available ring gains a `used_event` field (after the ring array) and the used ring gains an `avail_event` field. Instead of interrupting on every completion, the device only fires an interrupt when `used->idx` crosses the `used_event` threshold set by the driver. Similarly, the driver only sends a notification when `avail->idx` crosses the `avail_event` threshold set by the device. This reduces interrupt storms under heavy I/O. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: event index interrupt coalescing"`. Add notes directly in this TODO section.

- [ ] Negotiate `VIRTIO_F_RING_EVENT_IDX` (bit 29)
- [ ] Extend virtqueue allocation: +2 bytes for `used_event` at end of available ring, +2 bytes for `avail_event` at end of used ring
- [ ] After processing used ring: write `used_event = last_seen_used + batch_size` to available ring
- [ ] Before sending notification: check if `avail->idx` crossed `avail_event` from used ring
- [ ] Suppress unnecessary notifications when `avail_event` not crossed
- [ ] Tunable batch size via Registry: `HKLM\SYSTEM\Drivers\VirtIO\CoalesceCount`
- [ ] Commit: `"virtio-blk: event index interrupt coalescing"`

---

## 8. Packed Virtqueue (VirtIO 1.1+)

### 8.1 Packed Virtqueue Format

**Prompt:** Negotiate `VIRTIO_F_RING_PACKED` (bit 34). The packed virtqueue replaces the separate descriptor/available/used rings with a single unified ring, improving cache locality. Each packed descriptor is 16 bytes with `addr`, `len`, `id`, and `flags` fields — the `AVAIL` and `USED` bits in flags replace the separate rings. The driver and device track their own wrap counters. This is a significant architectural change from split virtqueues. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: packed virtqueue support"`. Add notes directly in this TODO section.

- [ ] Negotiate `VIRTIO_F_RING_PACKED` (bit 34) — mutual exclusive with split virtqueue
- [ ] Allocate single unified ring: `queue_size * 16` bytes
- [ ] Implement packed descriptor format:
  - [ ] `addr` (8 bytes), `len` (4 bytes), `id` (2 bytes), `flags` (2 bytes)
  - [ ] `VIRTQ_DESC_F_AVAIL` (bit 7) and `VIRTQ_DESC_F_USED` (bit 15) replace separate rings
- [ ] Track driver wrap counter and device wrap counter (toggle per ring wrap)
- [ ] Submission: set `AVAIL` flag matching driver wrap counter, write descriptor
- [ ] Completion: poll/check `USED` flag matching device wrap counter
- [ ] Notification: use packed notification format (optional suppression)
- [ ] Fallback: if not negotiated, use split virtqueue (current behavior)
- [ ] Commit: `"virtio-blk: packed virtqueue support"`

---

## 9. Secure Erase (VirtIO 1.2+)

### 9.1 Secure Erase Command

**Prompt:** Negotiate `VIRTIO_BLK_F_SECURE_ERASE` (bit 14). Read `max_secure_erase_sectors`, `max_secure_erase_seg`, and `secure_erase_sector_alignment` from device config. Implement `virtio_blk_secure_erase()` using `VIRTIO_BLK_T_SECURE_ERASE` (type 0x0E). This cryptographically erases sectors — useful for data sanitization before drive decommission or secure file deletion. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: secure erase"`. Add notes directly in this TODO section.

- [ ] Negotiate `VIRTIO_BLK_F_SECURE_ERASE` (bit 14)
- [ ] Read config: `max_secure_erase_sectors` (offset `0x3C`), `max_secure_erase_seg` (offset `0x40`), `secure_erase_sector_alignment` (offset `0x44`)
- [ ] Implement `virtio_blk_secure_erase(uint64_t sector, uint32_t num_sectors)`:
  - [ ] Build segment struct (same format as discard): `{ sector, num_sectors, flags = 0 }`
  - [ ] Build 3-descriptor chain: header (type = `0x0E`) + segment data + status
  - [ ] Split requests exceeding `max_secure_erase_sectors`
- [ ] Wire to secure-delete / drive-wipe utility
- [ ] Commit: `"virtio-blk: secure erase"`

---

## Priority Order

| Priority | Section                          | Description                                              |
|----------|----------------------------------|----------------------------------------------------------|
| 🔴 P0    | 3.1 MSI-X Interrupts            | Rules compliance — current driver uses legacy PIC        |
| 🔴 P0    | 2.2 Flush (Write Barriers)      | Data integrity — FAT32/IXFS need write barriers          |
| 🔴 P0    | 1.1 PCI Capability Discovery    | Foundation for all modern VirtIO features                |
| 🔴 P0    | 1.2 Modern Init Sequence        | Correct spec-compliant initialization                    |
| 🟠 P1    | 2.1 Block Size & Topology       | Correctness — 4K-sector drives break without this        |
| 🟠 P1    | 2.4 Read-Only Detection         | Correctness — prevent writes to RO devices               |
| 🟠 P1    | 2.3 Device Identification       | Feature — serial number for block device registry        |
| 🟠 P1    | 5.1 Device Reset & Recovery     | Production — recover from device errors and timeouts     |
| 🟡 P2    | 3.2 Async I/O Path              | Performance — unblocks CPU during disk I/O               |
| 🟡 P2    | 4.1 Discard (TRIM)              | SSD optimization — reclaim unused blocks                 |
| 🟡 P2    | 4.2 Write-Zeroes                | Performance — efficient large zeroing                    |
| 🟡 P2    | 5.2 Individual Queue Reset      | Less disruptive recovery than full device reset          |
| 🟢 P3    | 6.1 Per-CPU Request Queues      | Scalability — eliminates virtqueue lock contention       |
| 🟢 P3    | 7.1 Indirect Descriptors        | Scalability — large scatter-gather lists                 |
| 🟢 P3    | 7.2 Event Index (Coalescing)    | Performance — reduce interrupt storms                    |
| 🔵 P4    | 8.1 Packed Virtqueue            | Performance — better cache locality                      |
| 🔵 P4    | 9.1 Secure Erase                | Feature — cryptographic data sanitization                |

---

## OS Comparison

| Feature                          | 🪟 Windows 11 (viostor)              | 🐧 Linux (virtio-blk)                  | 🚀 Impossible OS                               |
| -------------------------------- | ------------------------------------- | ---------------------------------------- | ----------------------------------------------- |
| Basic read/write (split VQ)      | ✅                                     | ✅                                        | ✅ Done (MMIO, polling)                          |
| Modern PCI transport (caps)      | ✅ PCI caps discovery                  | ✅ PCI caps + MMIO fallback               | ⬜ §1.1 P0 — hardcoded MMIO                     |
| Feature negotiation              | ✅ Full VirtIO 1.0+                    | ✅ Full VirtIO 1.2                        | ⚠️ `VERSION_1` only — §2.1 P1                   |
| Flush (write barriers)           | ✅ Write cache flush                   | ✅ `REQ_OP_FLUSH`                         | ⬜ §2.2 P0 — no flush support                   |
| Block size / topology            | ✅ 4K-native aware                     | ✅ `blk_queue_physical_block_size()`      | ⬜ §2.1 P1 — assumes 512                        |
| Device ID (GET_ID)               | ✅                                     | ✅ `virtblk_get_id()`                     | ⬜ §2.3 P1                                      |
| Read-only detection              | ✅                                     | ✅ `set_disk_ro()`                        | ⬜ §2.4 P1                                      |
| MSI-X interrupts                 | ✅ Per-queue MSI-X                     | ✅ MSI-X / IOAPIC                         | ⬜ §3.1 P0 — uses legacy PIC                    |
| Async I/O (interrupt-driven)     | ✅ Overlapped I/O                      | ✅ `blk_mq_complete_request()`            | ⬜ §3.2 P2 — polling                            |
| Discard (TRIM)                   | ✅ Optimize Drives                     | ✅ `blk_queue_discard()`                  | ⬜ §4.1 P2                                      |
| Write-zeroes                     | ✅                                     | ✅ `REQ_OP_WRITE_ZEROES`                  | ⬜ §4.2 P2                                      |
| Error recovery / device reset    | ✅ Automatic retry + reset             | ✅ `virtio_break_device()` + reset        | ⬜ §5.1 P1 — no recovery                        |
| Individual queue reset           | ✅ VirtIO 1.2+                         | ✅ `virtqueue_reset()`                    | ⬜ §5.2 P2                                      |
| Multi-queue (`F_MQ`)             | ✅ Per-vCPU queues                     | ✅ `blk-mq` multi-queue                   | ⬜ §6.1 P3                                      |
| Indirect descriptors             | ✅                                     | ✅                                        | ⬜ §7.1 P3                                      |
| Event index (coalescing)         | ✅                                     | ✅                                        | ⬜ §7.2 P3                                      |
| Packed virtqueue                 | ✅ (newer builds)                      | ✅ `virtio_ring.c` packed path            | ⬜ §8.1 P4                                      |
| Secure erase                     | ✅ VirtIO 1.2+                         | ✅                                        | ⬜ §9.1 P4                                      |
| **MSI-X + MQ + async (default)** | ✅                                     | ✅                                        | ⬜ §3.1 + §6.1 + §3.2 — polling + single queue |
