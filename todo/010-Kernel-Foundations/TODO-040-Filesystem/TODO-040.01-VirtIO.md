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

> [!WARNING]
> **Endianness:** The VirtIO 1.x specification strictly enforces **little-endian** formatting for ALL multi-byte fields in all structures (`le16`, `le32`, `le64`) — descriptor addresses, ring indices, config registers, request headers. On x86-64 this is native, but all struct field types should use explicit `le16`/`le32`/`le64` typedefs to enforce correctness and future-proof for big-endian architectures.

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
  - [ ] Optionally record `PCI_CFG` (type 5) — fallback config access via PCI config cycles when BAR mapping fails
- [ ] Map the BAR(s) into kernel address space (identity-mapped MMIO)
- [ ] Read `notify_off_multiplier` from the notification capability's extended `virtio_pci_notify_cap` struct
- [ ] Compute per-queue notification address: `BAR_base + cap.offset + (queue_notify_off * notify_off_multiplier)`
- [ ] Note: if `notify_off_multiplier == 4096`, each queue gets its own page-aligned notification region (enables EPT/NPT hardware isolation of per-queue kicks)
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
- [ ] On unrecoverable init failure: set `FAILED` (bit 7 = 128) in `device_status` — hypervisor ceases processing
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

**Prompt:** Replace the current polling loop (`while (used->idx == last_seen_used)`) with interrupt-driven async I/O. Add a per-device completion event. The ISR fires when the device updates the used ring — it reads the completed descriptor heads and wakes waiting threads via `event_set()`. The submission path (`virtio_blk_read`/`virtio_blk_write`) uses `event_wait()` with a configurable timeout (5s default). Retain a polling fallback for pre-scheduler boot (before interrupts are available). Critically, insert strict memory barriers (`mfence` on x86-64) in the virtqueue submission and completion hot paths per the VirtIO spec. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: async interrupt-driven I/O"`. Add notes directly in this TODO section.

- [ ] Add `event_t io_completion` to `virtio_blk_dev` struct
- [ ] Queue ISR: read `used->idx`, process completed entries, call `event_set()`
- [ ] Config ISR: re-read device configuration (capacity, topology changes)
  - [ ] Check ISR status register: bit 0 = virtqueue data, bit 1 = config change (`VIRTIO_PCI_ISR_CONFIG`)
- [ ] Submission path: `event_wait(&io_completion, timeout_ms)` after virtqueue kick
- [ ] Process used ring entries: match `used_elem.id` to pending request, copy status byte
- [ ] Handle timeout: log error, attempt device reset (§5.1)
- [ ] Retain polling path: `if (!scheduler_running) { poll_used_ring(); }`
- [ ] Set `VIRTQ_AVAIL_F_NO_INTERRUPT = 0` in available ring flags (enable interrupts)
- [ ] **Memory barriers (critical for correctness):**
  - [ ] After writing descriptor chain to descriptor table → `smp_wmb()` before incrementing `avail->idx`
  - [ ] After incrementing `avail->idx` → `smp_wmb()` before writing notification register (kick)
  - [ ] After reading `used->idx` in ISR → `smp_rmb()` before reading `used->ring[]` entries
  - [ ] On x86-64: use `mfence` (full barrier) or `sfence`/`lfence` as appropriate
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

### 7.3 In-Order Completion

**Prompt:** Negotiate `VIRTIO_F_IN_ORDER` (bit 35). When negotiated, the device guarantees it will process, complete, and return descriptors to the used ring in the exact chronological order they were submitted to the available ring. This strict ordering eliminates the need for the driver to match arbitrary `used_elem.id` values to outstanding requests — it can simply reclaim descriptors sequentially, using a FIFO approach. This enables aggressive cache-coherent descriptor recycling: the driver can reuse the same descriptor slot immediately after it appears in the used ring, reducing TLB and cache pressure. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: in-order descriptor completion"`. Add notes directly in this TODO section.

- [ ] Negotiate `VIRTIO_F_IN_ORDER` (bit 35)
- [ ] When negotiated, switch used ring processing to sequential FIFO reclaim:
  - [ ] Remove per-request ID matching — descriptors return in submission order
  - [ ] Track a simple `next_expected_id` counter instead of a pending-request hash table
- [ ] Optimize descriptor recycling: reuse descriptor slots immediately after sequential completion
- [ ] Reduce cache pressure: no out-of-order descriptor table lookups
- [ ] Fallback: if not negotiated, use existing ID-matching completion path
- [ ] Commit: `"virtio-blk: in-order descriptor completion"`

### 7.4 Notification Data

**Prompt:** Negotiate `VIRTIO_F_NOTIFICATION_DATA` (bit 38). When negotiated, the notification write to the device changes from a simple 16-bit queue index to a richer 32-bit payload that includes additional state data. For split virtqueues, the driver must write: `(vqn & 0xFFFF) | (next_avail_idx << 16)`, packing the virtqueue number in the low 16 bits and the next available index in the high 16 bits. For packed virtqueues, the format is: `(vqn & 0xFFFF) | (next_avail_idx << 16) | (wrap_counter << 31)`. This extra data allows the host to optimize its polling strategy by knowing exactly where new descriptors begin, avoiding full ring scans. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: notification data"`. Add notes directly in this TODO section.

- [ ] Negotiate `VIRTIO_F_NOTIFICATION_DATA` (bit 38)
- [ ] Modify notification write path:
  - [ ] Split VQ: write `(vqn & 0xFFFF) | (next_avail_idx << 16)` instead of just `vqn`
  - [ ] Packed VQ: write `(vqn & 0xFFFF) | (next_avail_idx << 16) | (wrap_counter << 31)`
- [ ] Write to notification register as 32-bit MMIO instead of 16-bit
- [ ] Benefits: host avoids scanning entire ring to find new descriptors
- [ ] Fallback: if not negotiated, write 16-bit queue index (current behavior)
- [ ] Commit: `"virtio-blk: notification data"`

---

## 8. Packed Virtqueue (VirtIO 1.1+)

### 8.1 Packed Virtqueue Format

**Prompt:** Negotiate `VIRTIO_F_RING_PACKED` (bit 34). The packed virtqueue replaces the separate descriptor/available/used rings with a single unified ring, dramatically improving CPU cache locality by ensuring both driver and device read/write the same cache lines. Each packed descriptor is 16 bytes with `addr`, `len`, `id`, and `flags` fields — the `AVAIL` and `USED` bits in flags replace the separate rings. Both driver and device maintain internal boolean wrap counters (initialized to 1) that flip on every ring wraparound. This eliminates the Split VQ's problem of thrashing across 3 separate memory regions per I/O. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: packed virtqueue support"`. Add notes directly in this TODO section.

- [ ] Negotiate `VIRTIO_F_RING_PACKED` (bit 34) — mutually exclusive with split virtqueue
- [ ] Allocate single unified ring: `queue_size * 16` bytes (16-byte aligned)
- [ ] Implement packed descriptor format (`pvirtq_desc`):
  - [ ] `addr` (le64), `len` (le32), `id` (le16), `flags` (le16)
  - [ ] `VIRTQ_DESC_F_AVAIL` (bit 7) and `VIRTQ_DESC_F_USED` (bit 15) replace separate rings
- [ ] Wrap counter mechanism (both driver and device, initialized to `1`):
  - [ ] Counters flip `1 → 0 → 1` each time processing wraps past `queue_size`
  - [ ] Available: write `AVAIL = driver_wrap`, `USED = !driver_wrap`
  - [ ] Device detects available when descriptor's `AVAIL` matches its own wrap counter and `USED` is inverse
  - [ ] Completion: device sets both `AVAIL` and `USED` to its current wrap counter
  - [ ] Driver detects completion when `USED` bit matches `AVAIL` bit
- [ ] Submission: populate `addr`/`len`/`id`, set flags with correct AVAIL/USED bits, advance driver index
- [ ] Completion: scan ring for descriptors where USED matches driver's expected device wrap counter
- [ ] Notification: use packed notification format (optional suppression via event suppression struct)
- [ ] Fallback: if not negotiated, use split virtqueue (current behavior)
- [ ] Note: some advanced features (mergeable RX buffers, Jumbo MTU, USO) may be unsupported with packed VQ on certain controllers
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

## 10. Lifetime Metrics & Telemetry

### 10.1 Device Lifetime Information

**Prompt:** Negotiate `VIRTIO_BLK_F_LIFETIME` (bit 13). When negotiated, the device exposes wear-level and endurance metrics in the device config space — critical for SSD health monitoring in virtual environments. Read the lifetime fields to report estimated remaining device life. Expose metrics via Registry and wire to the Disk Manager GUI for drive health dashboards. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: lifetime metrics"`. Add notes directly in this TODO section.

- [ ] Negotiate `VIRTIO_BLK_F_LIFETIME` (bit 13)
- [ ] Read lifetime metric fields from device config space
- [ ] Store in `virtio_blk_dev.lifetime` struct
- [ ] Expose via Registry: `HKLM\HARDWARE\VirtIO\Block0\Lifetime\*`
- [ ] Wire to Disk Manager: display drive endurance / remaining life percentage
- [ ] Log: `[VirtIO] Block: device lifetime: %u%% remaining`
- [ ] Commit: `"virtio-blk: lifetime metrics"`

---

## 11. Zoned Block Storage (VirtIO 1.2+)

### 11.1 Zoned Block Device Support

**Prompt:** Negotiate `VIRTIO_BLK_F_ZONED` (bit 15). Zoned block devices (ZBDs) divide the disk into sequential-write-only zones — a model matching SMR (Shingled Magnetic Recording) drives and ZNS (Zoned Namespace) SSDs. When negotiated, the device exposes zone characteristics in the config space. Implement zone management commands: Report Zones, Open Zone, Close Zone, Finish Zone, Reset Zone, and Zone Append. This is a stretch goal for future compatibility with enterprise storage. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: zoned block device support"`. Add notes directly in this TODO section.

- [ ] Negotiate `VIRTIO_BLK_F_ZONED` (bit 15)
- [ ] Read zone config from device config space: `zoned.model`, `zoned.max_open_zones`, `zoned.max_active_zones`
- [ ] Implement zone management request types:
  - [ ] `VIRTIO_BLK_T_ZONE_REPORT` — enumerate zone descriptors (start LBA, length, condition, type)
  - [ ] `VIRTIO_BLK_T_ZONE_OPEN` — explicitly open a zone for writing
  - [ ] `VIRTIO_BLK_T_ZONE_CLOSE` — close zone, transition to closed state
  - [ ] `VIRTIO_BLK_T_ZONE_FINISH` — fill remaining capacity, transition to full
  - [ ] `VIRTIO_BLK_T_ZONE_RESET` — reset zone write pointer to start
  - [ ] `VIRTIO_BLK_T_ZONE_APPEND` — append data at write pointer (device returns actual LBA)
- [ ] Track per-zone write pointers and conditions in driver state
- [ ] Enforce sequential write constraint: reject random writes to sequential zones
- [ ] Commit: `"virtio-blk: zoned block device support"`

---

## 12. Adaptive Hybrid Polling (🚀 Impossible OS Exclusive)

### 12.1 Adaptive Interrupt/Polling Mode Switching

**Prompt:** Neither Windows viostor nor Linux virtio-blk implement adaptive hybrid polling for block devices. Linux has NAPI busy-polling for `virtio-net` but NOT for `virtio-blk`. Implement an adaptive I/O completion strategy that dynamically switches between three modes based on measured IOPS load:
- **Low load (< 1K IOPS):** Pure interrupt-driven (§3.2) — minimize CPU usage.
- **Medium load (1K–50K IOPS):** Hybrid — poll briefly after submission (configurable spin window, default 4 µs), then fall back to interrupt if no completion arrives. This catches fast completions without ISR overhead.
- **High load (> 50K IOPS):** Pure polling — disable interrupts (`VIRTQ_AVAIL_F_NO_INTERRUPT = 1`), spin on `used->idx` changes. At extreme IOPS, interrupt overhead dominates; polling amortizes the cost across many completions.
Track a rolling 100ms IOPS average to drive mode transitions. Expose the current mode and thresholds via Registry. This is a significant competitive advantage — no other OS adapts its VirtIO block completion strategy to workload in real time. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: adaptive hybrid polling"`. Add notes directly in this TODO section.

- [ ] Implement three I/O completion modes:
  - [ ] `VIRTIO_IO_MODE_INTERRUPT` — pure ISR-driven (§3.2 baseline)
  - [ ] `VIRTIO_IO_MODE_HYBRID` — brief spin (4 µs default), fallback to ISR
  - [ ] `VIRTIO_IO_MODE_POLL` — pure polling, interrupts disabled
- [ ] Track rolling IOPS: increment counter per completed I/O, compute average over 100ms window
- [ ] Mode transition logic (with hysteresis to prevent thrashing):
  - [ ] INTERRUPT → HYBRID: when IOPS > `low_threshold` (default 1000) for 3 consecutive windows
  - [ ] HYBRID → POLL: when IOPS > `high_threshold` (default 50000) for 3 consecutive windows
  - [ ] POLL → HYBRID: when IOPS < `high_threshold * 0.8` for 5 consecutive windows
  - [ ] HYBRID → INTERRUPT: when IOPS < `low_threshold * 0.8` for 5 consecutive windows
- [ ] Hybrid spin implementation: `rdtsc`-based busy-wait for `spin_us` microseconds checking `used->idx`
- [ ] Pure poll: set `VIRTQ_AVAIL_F_NO_INTERRUPT = 1`, process completions inline after submission
- [ ] Configurable via Registry:
  - [ ] `HKLM\SYSTEM\Drivers\VirtIO\AdaptivePolling\Enabled` (default true)
  - [ ] `HKLM\SYSTEM\Drivers\VirtIO\AdaptivePolling\LowThreshold` (default 1000)
  - [ ] `HKLM\SYSTEM\Drivers\VirtIO\AdaptivePolling\HighThreshold` (default 50000)
  - [ ] `HKLM\SYSTEM\Drivers\VirtIO\AdaptivePolling\SpinMicroseconds` (default 4)
- [ ] Expose current mode: `HKLM\HARDWARE\VirtIO\Block0\IoMode` = `"interrupt"` / `"hybrid"` / `"poll"`
- [ ] Log mode transitions: `[VirtIO] Block: I/O mode → HYBRID (IOPS=%u)`
- [ ] Commit: `"virtio-blk: adaptive hybrid polling"`

---

## 13. I/O Priority Queues (🚀 Impossible OS Exclusive)

### 13.1 Win32 I/O Priority to Virtqueue Mapping

**Prompt:** Windows exposes I/O priority levels (`IoPriorityVeryLow`, `IoPriorityLow`, `IoPriorityNormal`, `IoPriorityHigh`, `IoPriorityCritical`) but viostor treats all VirtIO requests equally — no priority differentiation at the device level. Linux has blk-mq priority hints but virtio-blk ignores them. Impossible OS can be the first OS to map Win32 I/O priority classes to dedicated virtqueues when multi-queue (`F_MQ`) is negotiated. Assign queue 0 = Critical/High, queue 1 = Normal, queue 2+ = Low/VeryLow (background). The hypervisor (QEMU/KVM) can then schedule higher-priority queues with lower latency. This gives Impossible OS the first true I/O QoS in a VirtIO driver — no other OS does this. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: I/O priority queue mapping"`. Add notes directly in this TODO section.

- [ ] Requires `VIRTIO_BLK_F_MQ` (§6.1) with ≥ 3 queues
- [ ] Define priority-to-queue mapping:
  - [ ] Queue 0: `IoPriorityCritical` + `IoPriorityHigh` (system, page faults, real-time)
  - [ ] Queue 1: `IoPriorityNormal` (standard user I/O)
  - [ ] Queue 2+: `IoPriorityLow` + `IoPriorityVeryLow` (background defrag, indexing, prefetch)
- [ ] In `virtio_blk_submit()`: read `current_thread->io_priority`, select target queue
- [ ] Assign independent MSI-X vectors per priority queue
- [ ] High-priority queue: process completions first in ISR (before normal/low queues)
- [ ] Expose metrics per priority level: `HKLM\HARDWARE\VirtIO\Block0\QueueStats\High\IOPS`
- [ ] Fallback: if `F_MQ` not available, treat all I/O as normal priority (single queue)
- [ ] Commit: `"virtio-blk: I/O priority queue mapping"`

---

## 14. Live Configuration Change & Hot-Resize

### 14.1 Config Change Notification Handling

**Prompt:** The VirtIO spec defines config change interrupts (ISR status bit 1 / MSI-X config vector) that fire when the hypervisor modifies the device configuration — most importantly, the `capacity` field can change at runtime (hot-resize). Neither Windows viostor nor Linux virtio-blk handle this gracefully — Linux logs "capacity changed" but doesn't notify userspace block layer until manual rescan. Implement a config change ISR that atomically re-reads the device config (using `config_generation` loop), detects capacity changes, and proactively notifies the filesystem layer and Disk Manager GUI. Also handle topology changes and write-cache mode changes. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: live config change handling"`. Add notes directly in this TODO section.

- [ ] Config change ISR (MSI-X vector 1 from §3.1):
  - [ ] Read ISR status register: check bit 1 (`VIRTIO_PCI_ISR_CONFIG`)
  - [ ] Atomic config read: loop on `config_generation` until stable
- [ ] Detect capacity change:
  - [ ] Compare new `capacity` with stored `vol->capacity`
  - [ ] If increased: `blkdev_update_capacity()` → notify VFS / filesystem
  - [ ] If decreased: log critical warning, mark excess region as offline
  - [ ] Update `Disk Manager` GUI: send resize event for live UI update
- [ ] Detect topology change: re-read `physical_block_exp`, `opt_io_size`
- [ ] Detect write-cache mode change: re-read `writeback` field
- [ ] Expose event: `HKLM\HARDWARE\VirtIO\Block0\LastConfigChange` = timestamp
- [ ] Log: `[VirtIO] Block: capacity changed %llu → %llu sectors (hot-resize)`
- [ ] Commit: `"virtio-blk: live config change handling"`

---

## 15. Hot-Plug & Hot-Unplug

### 15.1 PCI Device Hot-Plug/Unplug

**Prompt:** VirtIO PCI devices can be hot-plugged and hot-unplugged by the hypervisor at runtime. Implement PCI bus event handling: on device arrival, scan the new PCI function, run the full init sequence, register with the block device layer, and notify Disk Manager. On device removal (surprise or managed), quiesce all pending I/O, flush caches, tear down virtqueues, free all memory, unregister the block device, and update Disk Manager. Neither Windows viostor nor early Linux versions handle surprise removal gracefully — they often panic or leak memory. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: hot-plug/unplug support"`. Add notes directly in this TODO section.

- [ ] Hot-plug detection: PCI bus enumeration event or ACPI notification
  - [ ] Detect new VirtIO block PCI function (Vendor `0x1AF4`, Device `0x1042`/`0x1001`)
  - [ ] Run full `virtio_blk_init()` sequence on new device
  - [ ] Register new `blkdev` with disk subsystem
  - [ ] Notify Disk Manager GUI: new drive appeared
  - [ ] Log: `[VirtIO] Block: hot-plugged new device at PCI %02x:%02x.%x`
- [ ] Hot-unplug (managed removal):
  - [ ] Receive PCI removal notification
  - [ ] Quiesce: stop accepting new I/O requests, drain pending queue
  - [ ] Flush: `virtio_blk_flush()` → `scache_flush()`
  - [ ] Teardown: free virtqueue memory, release MSI-X vectors, unmap BARs
  - [ ] Unregister: `blkdev_unregister()`, remove from Disk Manager
  - [ ] Log: `[VirtIO] Block: device unregistered (managed removal)`
- [ ] Surprise removal (no notification):
  - [ ] Detect via MMIO read returning `0xFFFFFFFF` (PCI function gone)
  - [ ] Fail all pending I/O with `-ENODEV`
  - [ ] Cleanup without touching device registers
  - [ ] Log: `[VirtIO] Block: surprise removal detected — failing pending I/O`
- [ ] Commit: `"virtio-blk: hot-plug/unplug support"`

---

## 16. I/O Latency Telemetry (🚀 Impossible OS Exclusive)

### 16.1 Per-Request Nanosecond Latency Tracking

**Prompt:** No other OS exposes per-request I/O latency histograms at the VirtIO driver level. Linux has `blk_mq` latency stats but they're buried in sysfs and not virtio-specific. Windows viostor has no equivalent. Implement nanosecond-resolution I/O latency tracking using `rdtsc` / TSC: stamp each request at submission, stamp again at completion, compute delta. Maintain per-device latency histograms in logarithmic buckets (< 1µs, 1–10µs, 10–100µs, 100µs–1ms, 1–10ms, 10–100ms, > 100ms). Expose buckets via Registry for Disk Manager to render real-time latency charts. Also track: average latency, P50, P99, P99.9 percentiles, and max latency. This gives Impossible OS best-in-class storage observability — visible directly in the desktop GUI. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: I/O latency telemetry"`. Add notes directly in this TODO section.

- [ ] Stamp each I/O request at submission: `req->submit_tsc = rdtsc()`
- [ ] Stamp at completion: `req->complete_tsc = rdtsc()`
- [ ] Compute latency: `(complete_tsc - submit_tsc) * ns_per_tick`
- [ ] Calibrate TSC: compute `ns_per_tick` from `tsc_khz` (set during boot)
- [ ] Maintain histogram buckets (atomic increments, lockless):
  - [ ] `< 1 µs`, `1–10 µs`, `10–100 µs`, `100 µs–1 ms`, `1–10 ms`, `10–100 ms`, `> 100 ms`
- [ ] Track running statistics:
  - [ ] `total_requests`, `total_ns` (for average)
  - [ ] `min_ns`, `max_ns`
  - [ ] Approximate P50, P99, P99.9 using histogram interpolation
- [ ] Separate read vs write latency histograms
- [ ] Expose via Registry:
  - [ ] `HKLM\HARDWARE\VirtIO\Block0\Latency\Read\Avg_us`
  - [ ] `HKLM\HARDWARE\VirtIO\Block0\Latency\Read\P99_us`
  - [ ] `HKLM\HARDWARE\VirtIO\Block0\Latency\Read\Max_us`
  - [ ] `HKLM\HARDWARE\VirtIO\Block0\Latency\Read\Histogram\*`
  - [ ] Same for `Write`, `Flush`, `Discard`
- [ ] Wire to Disk Manager: real-time latency chart (bar histogram + percentile lines)
- [ ] Configurable: disable telemetry for zero-overhead production: `HKLM\SYSTEM\Drivers\VirtIO\LatencyTracking` (default enabled)
- [ ] Commit: `"virtio-blk: I/O latency telemetry"`

---

## 17. Predictive Sequential Prefetch (🚀 Impossible OS Exclusive)

### 17.1 Pattern-Based Read-Ahead

**Prompt:** Neither Windows viostor nor Linux virtio-blk implement driver-level read-ahead — they rely on the filesystem or block layer above. Implement a lightweight sequential access detector directly in the VirtIO block driver. Track the last N read offsets per open file handle. When 3+ consecutive reads are sequential (LBA[n+1] == LBA[n] + size[n]), trigger a speculative prefetch of the next `prefetch_sectors` worth of data into a small ring buffer. If the next read hits the prefetch buffer, return it immediately without a device round-trip. If the pattern breaks, silently discard the prefetch buffer. This gives sub-microsecond read latency for sequential workloads (file copy, media playback, database scans). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"virtio-blk: predictive sequential prefetch"`. Add notes directly in this TODO section.

- [ ] Track per-device read history: ring buffer of last 4 `(sector, count)` pairs
- [ ] Sequential detection: if `history[n].sector + history[n].count == history[n+1].sector` for 3+ entries
- [ ] On detection: issue asynchronous prefetch read of next `prefetch_sectors` (default 256 = 128 KB)
- [ ] Prefetch buffer: small PMM-allocated ring (default 512 KB, configurable)
- [ ] On read: check if requested `(sector, count)` falls within prefetch buffer
  - [ ] Hit: copy from prefetch buffer, return immediately (zero device latency)
  - [ ] Miss: discard prefetch buffer, issue normal read
- [ ] Pattern break: discard buffer, reset history
- [ ] Don't prefetch past device capacity or past EOF (if filesystem provides file-size hint)
- [ ] Track prefetch hit rate: `prefetch_hits / total_reads`
- [ ] Configurable via Registry:
  - [ ] `HKLM\SYSTEM\Drivers\VirtIO\Prefetch\Enabled` (default true)
  - [ ] `HKLM\SYSTEM\Drivers\VirtIO\Prefetch\SizeKB` (default 128)
  - [ ] `HKLM\SYSTEM\Drivers\VirtIO\Prefetch\MinSequential` (default 3)
- [ ] Expose: `HKLM\HARDWARE\VirtIO\Block0\PrefetchHitRate` = percentage
- [ ] Disable during random I/O workloads (database OLTP) — auto-detected by miss rate > 80%
- [ ] Commit: `"virtio-blk: predictive sequential prefetch"`

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
| 🟠 P1    | 14.1 Live Config Change         | Correctness — handle hot-resize and config changes       |
| 🟡 P2    | 3.2 Async I/O Path              | Performance — unblocks CPU during disk I/O               |
| 🟡 P2    | 4.1 Discard (TRIM)              | SSD optimization — reclaim unused blocks                 |
| 🟡 P2    | 4.2 Write-Zeroes                | Performance — efficient large zeroing                    |
| 🟡 P2    | 5.2 Individual Queue Reset      | Less disruptive recovery than full device reset          |
| 🟡 P2    | 15.1 Hot-Plug/Unplug            | Robustness — graceful device arrival/removal             |
| 🟢 P3    | 6.1 Per-CPU Request Queues      | Scalability — eliminates virtqueue lock contention       |
| 🟢 P3    | 7.1 Indirect Descriptors        | Scalability — large scatter-gather lists                 |
| 🟢 P3    | 7.2 Event Index (Coalescing)    | Performance — reduce interrupt storms                    |
| 🟢 P3    | 7.3 In-Order Completion         | Performance — optimized sequential descriptor recycling  |
| 🟢 P3    | 7.4 Notification Data           | Performance — host-side polling optimization             |
| 🟢 P3    | 10.1 Lifetime Metrics           | Monitoring — drive endurance in Disk Manager             |
| 🟢 P3    | 12.1 Adaptive Hybrid Polling    | 🚀 **Exclusive** — workload-adaptive completion strategy |
| 🟢 P3    | 13.1 I/O Priority Queues        | 🚀 **Exclusive** — Win32 I/O priority → virtqueue QoS    |
| 🟢 P3    | 16.1 I/O Latency Telemetry      | 🚀 **Exclusive** — ns-resolution histograms in GUI       |
| 🟢 P3    | 17.1 Predictive Prefetch        | 🚀 **Exclusive** — driver-level sequential read-ahead    |
| 🔵 P4    | 8.1 Packed Virtqueue            | Performance — better cache locality                      |
| 🔵 P4    | 9.1 Secure Erase                | Feature — cryptographic data sanitization                |
| 🔵 P4    | 11.1 Zoned Block Device         | Future — SMR/ZNS enterprise storage compatibility        |

---

## OS Comparison

| Feature                            | 🪟 Windows 11 (viostor)              | 🐧 Linux (virtio-blk)                  | 🚀 Impossible OS                               |
| ---------------------------------- | ------------------------------------- | ---------------------------------------- | ----------------------------------------------- |
| Basic read/write (split VQ)        | ✅                                     | ✅                                        | ✅ Done (MMIO, polling)                         |
| Modern PCI transport (caps)        | ✅ PCI caps discovery                  | ✅ PCI caps + MMIO fallback               | ⬜ §1.1 P0 — hardcoded MMIO                     |
| Feature negotiation                | ✅ Full VirtIO 1.0+                    | ✅ Full VirtIO 1.2                        | ⚠️ `VERSION_1` only — §2.1 P1                   |
| Flush (write barriers)             | ✅ Write cache flush                   | ✅ `REQ_OP_FLUSH`                         | ⬜ §2.2 P0 — no flush support                   |
| Block size / topology              | ✅ 4K-native aware                     | ✅ `blk_queue_physical_block_size()`      | ⬜ §2.1 P1 — assumes 512                        |
| Device ID (GET_ID)                 | ✅                                     | ✅ `virtblk_get_id()`                     | ⬜ §2.3 P1                                      |
| Read-only detection                | ✅                                     | ✅ `set_disk_ro()`                        | ⬜ §2.4 P1                                      |
| MSI-X interrupts                   | ✅ Per-queue MSI-X                     | ✅ MSI-X / IOAPIC                         | ⬜ §3.1 P0 — uses legacy PIC                    |
| Async I/O (interrupt-driven)       | ✅ Overlapped I/O                      | ✅ `blk_mq_complete_request()`            | ⬜ §3.2 P2 — polling                            |
| Memory barriers (VQ correctness)   | ✅ Implicit in WDF                     | ✅ `virtio_wmb()` / `virt_rmb()`          | ⬜ §3.2 P2 — no explicit barriers               |
| Discard (TRIM)                     | ✅ Optimize Drives                     | ✅ `blk_queue_discard()`                  | ⬜ §4.1 P2                                      |
| Write-zeroes                       | ✅                                     | ✅ `REQ_OP_WRITE_ZEROES`                  | ⬜ §4.2 P2                                      |
| Error recovery / device reset      | ✅ Automatic retry + reset             | ✅ `virtio_break_device()` + reset        | ⬜ §5.1 P1 — no recovery                        |
| Individual queue reset             | ✅ VirtIO 1.2+                         | ✅ `virtqueue_reset()`                    | ⬜ §5.2 P2                                      |
| Multi-queue (`F_MQ`)               | ✅ Per-vCPU queues                     | ✅ `blk-mq` multi-queue                   | ⬜ §6.1 P3                                      |
| Indirect descriptors               | ✅                                     | ✅                                        | ⬜ §7.1 P3                                      |
| Event index (coalescing)           | ✅                                     | ✅                                        | ⬜ §7.2 P3                                      |
| In-order completion                | ✅                                     | ✅ `VIRTIO_F_IN_ORDER`                    | ⬜ §7.3 P3                                      |
| Notification data                  | ✅                                     | ✅ `VIRTIO_F_NOTIFICATION_DATA`           | ⬜ §7.4 P3                                      |
| Packed virtqueue                   | ✅ (newer builds)                      | ✅ `virtio_ring.c` packed path            | ⬜ §8.1 P4                                      |
| Secure erase                       | ✅ VirtIO 1.2+                         | ✅                                        | ⬜ §9.1 P4                                      |
| Lifetime metrics                   | ✅ Health monitoring                   | ✅ `virtblk_attrs` sysfs                  | ⬜ §10.1 P3                                     |
| Zoned block device                 | ⬜ Not supported                       | ✅ `blk-zoned` + `virtblk_report_zones`   | ⬜ §11.1 P4                                     |
| Adaptive hybrid polling            | ⬜ Not implemented                     | ⬜ NAPI for net only, not blk             | ⬜ §12.1 P3 — **first for block devices**       |
| I/O priority → virtqueue QoS       | ⬜ Priority exists, no queue mapping   | ⬜ blk-mq hints ignored by virtio         | ⬜ §13.1 P3 — **first VirtIO QoS driver**       |
| Live config change (hot-resize)    | ⚠️ Manual rescan needed                | ⚠️ Logs change, no auto-resize            | ⬜ §14.1 P1 — **proactive auto-resize**         |
| Hot-plug / hot-unplug              | ✅ Basic                               | ✅ PCI hotplug                            | ⬜ §15.1 P2 — **graceful surprise removal**     |
| I/O latency telemetry (ns)         | ⬜ No driver-level histograms          | ⬜ sysfs block stats only (coarse)        | ⬜ §16.1 P3 — **real-time GUI histograms**      |
| Predictive sequential prefetch     | ⬜ Relies on filesystem cache          | ⬜ Relies on block layer readahead        | ⬜ §17.1 P3 — **driver-level prefetch**         |
| **MSI-X + MQ + async (default)**   | ✅                                     | ✅                                        | ⬜ §3.1 + §6.1 + §3.2 — polling + single queue  |
