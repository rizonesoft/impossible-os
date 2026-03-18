# 041-AHCI — Advanced Host Controller Interface

> **Goal:** Bring the existing AHCI SATA driver from basic polling read/write up to
> production-grade quality. Implement interrupt-driven I/O, Native Command Queuing (NCQ),
> hot-plug detection, error recovery, TRIM/discard, power management (DevSleep),
> MSI support, and BIOS/OS handoff — all per the AHCI 1.3.1 specification.
> The current driver (`src/kernel/drivers/ahci.c`, 824 lines) handles PCI detection,
> ABAR mapping, port init, DMA read/write, IDENTIFY, and ATAPI — all via polling.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL DMA buffers (command lists, FIS buffers, PRD tables, identify buffers). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). See `rules.md` Known Gotchas.

> [!IMPORTANT]
> **Spec Reference:** All section numbers, register offsets, and bit definitions reference the
> [AHCI 1.3.1 Specification](file:///home/derickpayne/impossible-os/specs/ahci-1.3.1.md)
> (Intel, 2012). The spec is 121 pages; a comprehensive summary is in the repo at `specs/ahci-1.3.1.md`.

---

## 1. Interrupt-Driven I/O

### 1.1 AHCI Interrupt Handler

**Prompt:** The current driver uses polling (`while ((port_read(PxCI) & (1 << slot))`) to wait for command completion — this wastes CPU cycles and blocks the calling thread. Replace with interrupt-driven I/O: register an IRQ handler for the AHCI PCI interrupt, enable `GHC.IE` (Global HBA Control, Interrupt Enable), and set `PxIE` (Port Interrupt Enable) bits for each active port. The ISR reads the global `IS` register to identify which ports fired, then reads `PxIS` to determine the cause (DHRS for D2H FIS, PSS for PIO Setup, etc.). Clear `PxIS` by writing-1-to-clear, then clear `IS`. Wake the blocked thread via a per-port completion event. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ahci: interrupt-driven I/O"`. Add notes directly in this TODO section.

- [ ] Register AHCI IRQ handler via `idt_register_handler()` (PCI interrupt line from config space)
- [ ] Enable `GHC.IE` (bit 1 of Global HBA Control, offset `04h`)
- [ ] Enable `PxIE` for each active port: at minimum `DHRS` (D2H Register FIS), `PSS` (PIO Setup), `DSS` (DMA Setup), `SDBS` (Set Device Bits), `TFES` (Task File Error)
- [ ] ISR: read `IS` → for each set bit, read `PxIS` → handle completion/error → clear `PxIS` → clear `IS`
- [ ] Add per-port `event_t completion` — `event_wait()` in `port_issue_cmd()`, `event_set()` in ISR
- [ ] Remove polling loop from `port_issue_cmd()`
- [ ] Retain a polling fallback with configurable timeout (5s) for pre-scheduler boot
- [ ] Commit: `"ahci: interrupt-driven I/O"`

### 1.2 MSI / MSI-X Support

**Prompt:** PCI Message Signaled Interrupts (MSI) are faster and more reliable than legacy INTx pin-based interrupts — they avoid IRQ sharing and spurious interrupts. Check the PCI Capabilities List for an MSI capability (Cap ID `0x05`) or MSI-X capability (Cap ID `0x11`). If found, program the MSI Message Address and Message Data registers to target a specific IDT vector. Disable legacy INTx via PCI Command Register bit 10. QEMU's ICH9 AHCI controller supports MSI. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ahci: MSI interrupt support"`. Add notes directly in this TODO section.

- [ ] Walk PCI Capabilities List to find MSI capability (Cap ID `0x05`, offset varies)
- [ ] Read MSI Control Register: determine 64-bit capable, max vectors
- [ ] Program MSI Message Address (`0xFEE00000 | (cpu << 12)`) and Message Data (IDT vector)
- [ ] Enable MSI via MSI Control Register Enable bit
- [ ] Disable legacy INTx: set PCI Command Register bit 10 (`Interrupt Disable`)
- [ ] Register IDT handler for the MSI vector instead of legacy IRQ
- [ ] Fallback: if MSI not available, use legacy INTx (current behavior)
- [ ] Commit: `"ahci: MSI interrupt support"`

---

## 2. Native Command Queuing (NCQ)

### 2.1 NCQ Read/Write (FPDMA)

**Prompt:** Native Command Queuing allows up to 32 concurrent I/O requests per port, enabling the drive to reorder them for optimal performance (elevator algorithm). NCQ uses FPDMA (First Party DMA) commands: `READ FPDMA QUEUED` (0x60) and `WRITE FPDMA QUEUED` (0x61). Each command uses a unique tag (0–31) written to the count register bits 7:3. The drive reports completion via Set Device Bits FIS, which sets bits in `PxSACT`. The ISR checks `PxSACT` to determine which tags completed. Check `CAP.SNCQ` to verify NCQ support, and read `CAP.NCS` for the number of command slots. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ahci: NCQ read/write (FPDMA)"`. Add notes directly in this TODO section.

- [ ] Check `CAP.SNCQ` (bit 30) — verify HBA supports NCQ
- [ ] Read `CAP.NCS` (bits 12:8) — number of command slots (0-based, max 32)
- [ ] Implement `ahci_ncq_read(port, lba, count, buf, tag)` using `READ FPDMA QUEUED` (0x60)
- [ ] Implement `ahci_ncq_write(port, lba, count, buf, tag)` using `WRITE FPDMA QUEUED` (0x61)
- [ ] FIS setup: command in Features register, LBA in standard LBA fields, tag in Count bits 7:3
- [ ] Set `PxSACT` bit for the tag before setting `PxCI` bit
- [ ] ISR: on `SDBS` (Set Device Bits) interrupt, read `PxSACT` to find completed tags
- [ ] Tag allocation: simple bitmap with `find_first_zero_bit()` per port
- [ ] Async API: `ahci_submit(port, lba, count, buf, is_write, callback)` — returns tag
- [ ] Fallback: if `CAP.SNCQ == 0`, use existing sequential DMA read/write
- [ ] Commit: `"ahci: NCQ read/write (FPDMA)"`

### 2.2 Interrupt Coalescing

**Prompt:** Under heavy I/O workloads, per-command interrupts cause interrupt storms. AHCI provides Command Completion Coalescing (CCC) via the `CCC_CTL` (offset `14h`) and `CCC_PORTS` (offset `18h`) registers. Check `CAP.CCCS` (bit 7) for support. Program `CCC_CTL` with a command count threshold and a timeout (e.g., 4 commands or 1ms, whichever first). Assign ports to the CCC group via `CCC_PORTS`. The CCC interrupt uses the vector specified in `CCC_CTL.INT`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ahci: interrupt coalescing"`. Add notes directly in this TODO section.

- [ ] Check `CAP.CCCS` (bit 7) — HBA supports Command Completion Coalescing
- [ ] Read `CCC_CTL` (offset `14h`): TV (timeout value, 1ms units), CC (command count)
- [ ] Program `CCC_CTL`: set timeout (e.g., 1ms), command count (e.g., 4), enable
- [ ] Program `CCC_PORTS` (offset `18h`): include all active ports in CCC group
- [ ] ISR: check CCC interrupt vector in addition to per-port vectors
- [ ] Tunable via Registry: `HKLM\SYSTEM\Drivers\AHCI\CoalesceTimeoutMs` and `CoalesceCount`
- [ ] Commit: `"ahci: interrupt coalescing"`

---

## 3. Error Recovery

### 3.1 Command List Override (CLO)

**Prompt:** When a SATA device becomes unresponsive (e.g., bad sector, cable glitch), the port's `PxTFD.STS.BSY` or `PxTFD.STS.DRQ` bits get stuck at 1, blocking all further commands. AHCI provides Command List Override (`PxCMD.CLO`, bit 3) to forcefully clear these bits. Check `CAP.SCLO` (bit 24) for support. The recovery sequence: stop command engine (`PxCMD.ST = 0`), set `PxCMD.CLO = 1`, wait for CLO to auto-clear, then restart the command engine. After CLO, issue a COMRESET (write `PxSCTL.DET = 1`, wait 1ms, write `DET = 0`) to re-establish the link. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ahci: error recovery with CLO"`. Add notes directly in this TODO section.

- [ ] Check `CAP.SCLO` (bit 24) — HBA supports Command List Override
- [ ] Detect stuck port: `PxTFD.STS.BSY == 1 || PxTFD.STS.DRQ == 1` after timeout
- [ ] Recovery sequence:
  - [ ] Stop command engine: `PxCMD.ST = 0`, wait for `PxCMD.CR = 0`
  - [ ] Set `PxCMD.CLO = 1`, poll until CLO auto-clears to 0
  - [ ] Issue COMRESET: write `PxSCTL.DET = 1`, wait 1ms, write `DET = 0`
  - [ ] Wait for `PxSSTS.DET = 3` (device present + communication established)
  - [ ] Clear `PxSERR` (write all-ones to clear error bits)
  - [ ] Restart command engine: `PxCMD.ST = 1`
- [ ] Retry failed commands (up to 3 retries) before reporting error
- [ ] Log: `[AHCI] Port X: CLO recovery — BSY/DRQ stuck, link reset`
- [ ] Commit: `"ahci: error recovery with CLO"`

### 3.2 Port Error Handling

**Prompt:** AHCI defines multiple error conditions reported via `PxIS` and `PxSERR`. Fatal errors (HBFS, HBDS, IFS, TFES) require stopping the port, clearing errors, and restarting. Non-fatal errors (INFS, OFS) are logged but allow continued operation. Implement a comprehensive error handler that classifies errors, attempts recovery, and reports unrecoverable failures to the block device layer. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ahci: comprehensive port error handling"`. Add notes directly in this TODO section.

- [ ] Classify `PxIS` error bits:
  - [ ] Fatal: `HBFS` (Host Bus Fatal), `HBDS` (Host Bus Data), `IFS` (Interface Fatal), `TFES` (Task File Error)
  - [ ] Non-fatal: `INFS` (Interface Non-Fatal), `OFS` (Overflow)
- [ ] On fatal error: stop DMA, clear `PxSERR`, clear `PxIS`, attempt CLO + COMRESET recovery
- [ ] On non-fatal error: log warning, clear error bits, continue operation
- [ ] Parse `PxSERR` for detailed error info:
  - [ ] `DIAG.X` = exchange (hot-plug), `DIAG.N` = PhyRdy change, `ERR.E` = internal error
- [ ] Track per-port error counters (CRC errors, link resets, command failures)
- [ ] Expose error counters via Registry: `HKLM\HARDWARE\AHCI\PortX\Errors\*`
- [ ] Commit: `"ahci: comprehensive port error handling"`

---

## 4. Hot-Plug Support

### 4.1 Hot-Plug Detection

**Prompt:** AHCI supports native hot-plug for eSATA ports and hot-swap bays. When a drive is inserted or removed, the HBA fires `PxIS.PCS` (Port Connect Change) and `PxIS.PRCS` (PhyRdy Change). Check `CAP.SXS` (bit 5) for external SATA support, `CAP.SMPS` (bit 28) for mechanical presence switch. On hot-plug: detect device signature, initialize the port, run IDENTIFY, probe for partitions. On hot-unplug: flush dirty buffers, unmount filesystems, release resources. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ahci: hot-plug detection"`. Add notes directly in this TODO section.

- [ ] Enable hot-plug interrupts: `PxIE.PCE` (Port Connect) and `PxIE.PRCE` (PhyRdy Change)
- [ ] ISR: on `PxIS.PCS` or `PxIS.PRCS`, check `PxSSTS.DET`:
  - [ ] `DET = 3` (device present): hot-plug insertion
  - [ ] `DET = 0` (no device): hot-plug removal
- [ ] Hot-plug insertion sequence:
  - [ ] Clear `PxSERR` error bits
  - [ ] Wait for `PxTFD.STS.BSY = 0` (device ready)
  - [ ] Read device signature from `PxSIG` (SATA vs ATAPI)
  - [ ] Run `ahci_do_identify()` → model, serial, capacity
  - [ ] Probe partitions → auto-mount FAT32/IXFS
  - [ ] Notify desktop: toast notification "Drive detected — D:\\ (500 GB, SATA)"
- [ ] Hot-plug removal sequence:
  - [ ] Stop command engine for port
  - [ ] Flush any dirty buffers to disk (if still responsive)
  - [ ] Unmount all filesystems on the device
  - [ ] Release port resources
  - [ ] Notify desktop: toast notification "Drive removed — D:\\"
- [ ] QEMU test: use `-device ahci,id=ahci0 -device ide-hd,drive=disk1,bus=ahci0.0,removable=on`
- [ ] Commit: `"ahci: hot-plug detection"`

### 4.2 Staggered Spin-Up

**Prompt:** In multi-drive systems, spinning up all drives simultaneously draws excessive current. Check `CAP.SSS` (bit 27) for staggered spin-up support. When supported, the HBA keeps drives powered off at boot; the driver must explicitly set `PxCMD.SUD` (Spin-Up Device) for each port sequentially, pausing between each spin-up to limit inrush current. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ahci: staggered spin-up"`. Add notes directly in this TODO section.

- [ ] Check `CAP.SSS` (bit 27) — staggered spin-up supported
- [ ] If supported: do NOT set `PxCMD.SUD` for all ports simultaneously
- [ ] Sequential spin-up: for each port with device present:
  - [ ] Set `PxCMD.SUD = 1`
  - [ ] Wait for `PxSSTS.DET = 3` (device present + link established)
  - [ ] Wait for `PxTFD.STS.BSY = 0` (device ready)
  - [ ] Delay 100ms before spinning up next port
- [ ] If `CAP.SSS == 0`: spin-up all ports simultaneously (current behavior)
- [ ] Commit: `"ahci: staggered spin-up"`

---

## 5. TRIM / Discard

### 5.1 DATA SET MANAGEMENT (TRIM)

**Prompt:** TRIM notifies SSDs that deleted blocks can be erased internally, maintaining write performance over time. TRIM uses the ATA `DATA SET MANAGEMENT` command (0x06). Check IDENTIFY DEVICE word 169 bit 0 for TRIM support. Build a range descriptor list in a 512-byte sector (each entry: 6-byte LBA + 2-byte count). Issue with the TRIM bit set in the Features register. Expose as `ahci_trim(port, lba, count)` for the filesystem layer to call on file deletion. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ahci: TRIM / discard support"`. Add notes directly in this TODO section.

- [ ] Check IDENTIFY DEVICE word 169 bit 0 — device supports TRIM
- [ ] Check IDENTIFY DEVICE word 69 bit 14 — supports deterministic read after TRIM
- [ ] Build TRIM range descriptor: array of `{ uint64_t lba : 48; uint16_t count; }`
- [ ] Pack up to 64 range entries per 512-byte sector (8 bytes each)
- [ ] Issue `DATA SET MANAGEMENT` command (0x06) with TRIM bit in Features register
- [ ] Implement `ahci_trim(int port, uint64_t lba, uint32_t count)` — public API
- [ ] Wire to VFS: `fat32_unlink()` and `ixfs_delete()` call `blkdev_discard()` → `ahci_trim()`
- [ ] Commit: `"ahci: TRIM / discard support"`

---

## 6. Power Management

### 6.1 Interface Power Management (Partial / Slumber)

**Prompt:** AHCI supports SATA link power states: Partial (fast wake, ~10µs) and Slumber (deep sleep, ~10ms wake). These are controlled via `PxCMD.ICC` (Interface Communication Control) and enabled via `CAP.SALP` (Aggressive Link Power Management). When a port is idle for a configurable timeout (e.g., 500ms), transition the link to Partial. After a longer idle (e.g., 5s), transition to Slumber. Wake automatically on the next command. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ahci: link power management"`. Add notes directly in this TODO section.

- [ ] Check `CAP.SALP` (bit 26) — supports Aggressive Link Power Management
- [ ] Enable ALPM: set `PxCMD.ALPE` (bit 26) and `PxCMD.ASP` (bit 27)
- [ ] Idle timeout → write `PxCMD.ICC = 2` (Partial) or `ICC = 6` (Slumber)
- [ ] Auto-wake: any new command automatically transitions link back to Active
- [ ] Tunable via Registry: `HKLM\SYSTEM\Drivers\AHCI\PowerPolicy` (Performance / Balanced / Power Saver)
- [ ] Commit: `"ahci: link power management"`

### 6.2 Device Sleep (DevSleep) — AHCI 1.3.1

**Prompt:** DevSleep is the deepest SATA power state, introduced in AHCI 1.3.1. Both the host and device PHYs shut down completely, drawing < 5mW. Check `CAP2.SDS` (bit 3) and `CAP2.SADM` (bit 4) for hardware support. The entry/exit is controlled by `PxDEVSLP` register: `ADSE` enables automatic entry, `DITO` sets the idle timeout, `DETO` sets the exit timeout, and `MDAT` sets the minimum device attention time. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ahci: DevSleep power management"`. Add notes directly in this TODO section.

- [ ] Check `CAP2.SDS` (bit 3, offset `24h`) — DevSleep supported
- [ ] Check `CAP2.SADM` (bit 4) — supports automatic DevSleep management
- [ ] Check IDENTIFY DEVICE word 78 bit 8 — device supports DevSleep
- [ ] Configure `PxDEVSLP` register:
  - [ ] `ADSE` (bit 0) — enable Automatic Device Sleep
  - [ ] `DITO` (bits 24:15) — Device Idle Timeout (in 1ms units, e.g., 500ms)
  - [ ] `DETO` (bits 7:2) — Device Exit Timeout (from device IDENTIFY data)
  - [ ] `MDAT` (bits 14:10) — Minimum Device Attention Time
- [ ] Auto-entry: hardware enters DevSleep after DITO ms of idle
- [ ] Auto-exit: hardware wakes on next command, waits DETO ms for device ready
- [ ] Tunable: `HKLM\SYSTEM\Drivers\AHCI\DevSleepIdleMs` (default 500)
- [ ] Commit: `"ahci: DevSleep power management"`

---

## 7. BIOS/OS Handoff

### 7.1 BOHC (BIOS/OS Handoff Control)

**Prompt:** During boot, the BIOS owns the AHCI controller. The OS must request ownership via the BOHC register (offset `28h`). Set `BOHC.OOS` (OS Ownership) to 1, then wait for `BOHC.BOS` (BIOS Ownership) to clear. If the BIOS doesn't release within 25ms, set `BOHC.OOC` (OS Ownership Change) and forcibly take control. Without proper handoff, SMM firmware may interfere with OS disk operations. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ahci: BIOS/OS handoff"`. Add notes directly in this TODO section.

- [ ] Check `CAP2.BOH` (bit 0, offset `24h`) — BIOS/OS handoff supported
- [ ] If supported:
  - [ ] Set `BOHC.OOS` (bit 1, offset `28h`) — request OS ownership
  - [ ] Wait up to 25ms for `BOHC.BOS` (bit 0) to clear
  - [ ] If timeout: set `BOHC.OOC` (bit 3) — force ownership change
  - [ ] Wait additional 2s for BIOS cleanup (per spec recommendation)
- [ ] Log: `[AHCI] BIOS/OS handoff complete` or `[AHCI] BOHC not supported — skipping handoff`
- [ ] Commit: `"ahci: BIOS/OS handoff"`

---

## 8. Enclosure Management

### 8.1 Activity LED & Drive Identification

**Prompt:** AHCI supports enclosure management for server/NAS environments. The `EM_LOC` (offset `1Ch`) and `EM_CTL` (offset `20h`) registers control LED indicators on drive bays. Check `CAP.EMS` (bit 6) for support. Implement LED messaging: Locate (blink to identify a drive bay), Fault (red LED for failed drive), Activity (on during I/O). This is useful for the graphical Disk Manager tool. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ahci: enclosure management LEDs"`. Add notes directly in this TODO section.

- [ ] Check `CAP.EMS` (bit 6) — enclosure management supported
- [ ] Read `EM_LOC` (offset `1Ch`) — location of enclosure management message buffer
- [ ] Read `EM_CTL` (offset `20h`) — capabilities and control
- [ ] Implement LED message types:
  - [ ] `ahci_led_locate(port, on)` — blink locate LED
  - [ ] `ahci_led_fault(port, on)` — red fault LED
  - [ ] `ahci_led_activity(port)` — auto-toggle during I/O
- [ ] Expose to Disk Manager GUI: "Identify Drive" button → blink locate LED
- [ ] Commit: `"ahci: enclosure management LEDs"`

---

## 9. Multi-Port & ATAPI Improvements

### 9.1 Port Multiplier Support

**Prompt:** AHCI supports Port Multipliers (PM) which allow a single host port to fan out to multiple SATA drives (up to 15 per PM). Check `CAP.SPM` (bit 17) for support. When a PM is detected (via SATA signature `0x96690101`), enumerate downstream ports by reading the PM's GSCR (General Status and Control Registers) via Register FIS. Each downstream device uses a different PM port number in the FIS. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ahci: port multiplier support"`. Add notes directly in this TODO section.

- [ ] Check `CAP.SPM` (bit 17) — port multiplier support
- [ ] Detect PM: `PxSIG == 0x96690101` (port multiplier signature)
- [ ] Read PM GSCR registers via Register H2D FIS with PM port field
- [ ] Enumerate downstream ports: scan PM port 0–15, check for device presence
- [ ] Route I/O: set FIS PM port field for each downstream device
- [ ] Track PM topology: `ahci_port.pm_ports[]` array of downstream devices
- [ ] Commit: `"ahci: port multiplier support"`

### 9.2 ATAPI Enhanced Support

**Prompt:** The existing ATAPI support handles basic READ(10) and IDENTIFY PACKET DEVICE. Add full ATAPI support: TEST UNIT READY, REQUEST SENSE (for error details), READ TOC (CD table of contents), EJECT (START STOP UNIT with LoEj=1), and GET CONFIGURATION. Expose via a `cdrom_*` API that the VFS can use for ISO 9660 mounting. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ahci: enhanced ATAPI/CD-ROM support"`. Add notes directly in this TODO section.

- [ ] Implement SCSI commands via `atapi_packet_cmd()`:
  - [ ] `TEST UNIT READY` (0x00) — check if media is present
  - [ ] `REQUEST SENSE` (0x03) — get detailed error information
  - [ ] `READ TOC` (0x43) — read CD-ROM table of contents
  - [ ] `START STOP UNIT` (0x1B) with LoEj=1 — eject optical media
  - [ ] `GET CONFIGURATION` (0x46) — get feature profiles (CD-R, DVD, etc.)
- [ ] Implement `cdrom_eject(port)`, `cdrom_is_present(port)`, `cdrom_read_toc(port)`
- [ ] Register ATAPI devices with block device layer (sector size = 2048)
- [ ] Wire to File Manager: right-click drive → "Eject"
- [ ] Commit: `"ahci: enhanced ATAPI/CD-ROM support"`

---

## 10. SMART & Health Monitoring

### 10.1 SMART Attribute Reading

**Prompt:** Self-Monitoring, Analysis and Reporting Technology (SMART) allows the OS to read drive health status. Issue ATA `SMART READ DATA` (0xB0, feature 0xD0) to get the 512-byte attribute table. Parse key attributes: Reallocated Sectors (ID 5), Power-On Hours (ID 9), Temperature (ID 194), Pending Sectors (ID 197). Expose via Registry at `HKLM\HARDWARE\AHCI\PortX\SMART\*`. The Disk Manager GUI can display drive health. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"ahci: SMART health monitoring"`. Add notes directly in this TODO section.

- [ ] Issue `SMART READ DATA` command (0xB0, feature 0xD0, LBA mid `0x4F`, LBA hi `0xC2`)
- [ ] Parse 512-byte attribute table (30 entries × 12 bytes each, starting at offset 2)
- [ ] Extract key attributes:
  - [ ] ID 5: Reallocated Sector Count (> 0 = degraded)
  - [ ] ID 9: Power-On Hours
  - [ ] ID 194: Temperature (°C)
  - [ ] ID 197: Current Pending Sector Count (> 0 = warning)
  - [ ] ID 198: Offline Uncorrectable Sector Count
- [ ] Issue `SMART RETURN STATUS` (0xB0, feature 0xDA) — pass/fail threshold check
- [ ] Implement `ahci_smart_read(port, buf)` and `ahci_smart_status(port)` public API
- [ ] Expose via Registry: `HKLM\HARDWARE\AHCI\PortX\SMART\Temperature`, etc.
- [ ] Commit: `"ahci: SMART health monitoring"`

---

## Priority Order

| Priority | Section                       | Description                                        |
|----------|-------------------------------|----------------------------------------------------|
| 🔴 P0    | 1.1 Interrupt-Driven I/O      | Eliminates CPU-wasting polling — foundational      |
| 🔴 P0    | 3.1 Command List Override     | Required for any error recovery — drives get stuck |
| 🟠 P1    | 2.1 NCQ (FPDMA)              | Major performance gain for concurrent I/O          |
| 🟠 P1    | 3.2 Port Error Handling       | Production systems need robust error handling      |
| 🟠 P1    | 5.1 TRIM / Discard           | SSD performance degrades without TRIM              |
| 🟠 P1    | 7.1 BIOS/OS Handoff          | Prevents SMM firmware interference                 |
| 🟡 P2    | 4.1 Hot-Plug Detection       | eSATA and hot-swap bay support                     |
| 🟡 P2    | 1.2 MSI Support              | Better interrupt routing (no IRQ sharing)          |
| 🟡 P2    | 6.1 Link Power Management    | Battery life on laptops with SATA SSDs             |
| 🟡 P2    | 10.1 SMART Monitoring        | Drive health visible in Disk Manager               |
| 🟢 P3    | 2.2 Interrupt Coalescing     | Performance under heavy I/O workloads              |
| 🟢 P3    | 6.2 DevSleep (AHCI 1.3.1)   | Ultra-low-power idle for laptops                   |
| 🟢 P3    | 9.2 Enhanced ATAPI/CD-ROM   | Full CD/DVD support                                |
| 🟢 P3    | 4.2 Staggered Spin-Up       | Multi-drive server/NAS environments                |
| 🔵 P4    | 8.1 Enclosure Management    | Server rack LED identification                     |
| 🔵 P4    | 9.1 Port Multiplier         | External multi-drive enclosures                    |

---

## OS Comparison

| Feature                         | 🪟 Windows 11 (StorAHCI)          | 🐧 Linux (libata / ahci.c)           | 🚀 Impossible OS                              |
| ------------------------------- | --------------------------------- | ------------------------------------ | --------------------------------------------- |
| Basic AHCI read/write           | ✅                                 | ✅                                    | ✅ Done (polling DMA)                          |
| IDENTIFY DEVICE                 | ✅                                 | ✅                                    | ✅ Done                                        |
| ATAPI / CD-ROM                  | ✅ Full SCSI passthrough           | ✅ Full (sr, sg)                      | ⚠️ Basic (READ, IDENTIFY only) — §9.2         |
| Interrupt-driven I/O            | ✅ MSI-X multi-queue               | ✅ MSI / per-port IRQ                 | ⬜ §1.1 P0 — currently polling                |
| MSI / MSI-X                     | ✅ MSI-X preferred                 | ✅ MSI / MSI-X                        | ⬜ §1.2 P2                                    |
| Native Command Queuing (NCQ)    | ✅ 32-deep queue                   | ✅ 32-deep, auto-detect               | ⬜ §2.1 P1 — sequential only                  |
| Interrupt coalescing            | ✅ Adaptive                        | ✅ CCC support                        | ⬜ §2.2 P3                                    |
| Error recovery (CLO)            | ✅ Automatic retry + CLO           | ✅ libata EH (error handler)          | ⬜ §3.1 P0 — no recovery                      |
| Comprehensive error handling    | ✅ WHEA integration                | ✅ libata error handler               | ⬜ §3.2 P1                                    |
| Hot-plug (eSATA / swap bay)     | ✅ Full hot-plug                   | ✅ Full hot-plug                       | ⬜ §4.1 P2                                    |
| Staggered spin-up               | ✅                                 | ✅                                    | ⬜ §4.2 P3                                    |
| TRIM / discard                  | ✅ Optimize Drives                 | ✅ `fstrim`, auto-discard             | ⬜ §5.1 P1                                    |
| Link power management (ALPM)    | ✅ Balanced / Performance          | ✅ `min_power` / `med_power_with_dipm` | ⬜ §6.1 P2                                    |
| DevSleep (AHCI 1.3.1)           | ✅ Connected Standby               | ✅ Supported                           | ⬜ §6.2 P3                                    |
| BIOS/OS handoff (BOHC)          | ✅                                 | ✅                                    | ⬜ §7.1 P1                                    |
| Enclosure management (LEDs)     | ✅ enclosure aware                 | ✅ `ledtrig-disk`                      | ⬜ §8.1 P4                                    |
| Port multiplier                 | ✅ (limited)                       | ✅ `libata-pmp`                        | ⬜ §9.1 P4                                    |
| SMART monitoring                | ✅ Storage Spaces / CrystalDisk    | ✅ `smartctl` (smartmontools)          | ⬜ §10.1 P2                                   |
| **NCQ + IRQ-driven (default)**  | ✅                                 | ✅                                    | ⬜ §1.1 + §2.1 — polling today                |
