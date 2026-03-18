# 040.03-ATAPI-SCSI-MMC — ATAPI Optical Drive & SCSI MMC Driver

> **Goal:** Implement a production-grade ATAPI driver that tunnels SCSI Multimedia
> Commands (MMC) over the ATA transport layer for CD-ROM, CD-RW, DVD-ROM, and
> Blu-ray optical drives. Support both legacy PATA (PIO + Bus Master DMA) and
> modern AHCI transports via a clean transport abstraction layer. Implement the
> core SCSI MMC command set (INQUIRY, READ CAPACITY, READ, TOC, media control),
> full sense data error handling, removable media event detection, and filesystem
> integration for ISO 9660 / Joliet / UDF volumes.
> The driver registers as a block device under `D:\` or assigned drive letter and
> exposes the optical drive through the Windows-style Device Manager and Disk Manager.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for DMA buffers (PRDT, sense data, read buffers). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). See `rules.md` Known Gotchas.

> [!WARNING]
> **Endianness:** SCSI payloads and responses are strictly **Big-Endian (Network Byte Order)**, while x86-64 is Little-Endian. All multi-byte SCSI response fields **must** be byte-swapped (`bswap32` / `bswap16`) before interpretation. Failure to byte-swap will produce wildly incorrect LBA addresses and capacities.

> [!IMPORTANT]
> **Spec Reference:** All section numbers, register offsets, CDB formats, and sense codes reference the
> [ATAPI & SCSI MMC Specification](file:///home/derickpayne/impossible-os/specs/atapi-scsi-mmc.md)
> in the repo at `specs/atapi-scsi-mmc.md`.

---

## 1. Device Discovery & Signature Detection

### 1.1 ATAPI Signature Evaluation

**Prompt:** After a hardware reset, software reset (SRST), or EXECUTE DEVICE DIAGNOSTIC, ATA/ATAPI devices place a signature into the task file registers. The kernel must read LBA Mid and LBA High to detect the ATAPI signature (`0x14`, `0xEB`). This is the only standardized method to differentiate optical/tape drives from magnetic hard drives. Never use the Status register alone — it fails on modern hardware. Under AHCI, read the port's Signature register (`PxSIG`). If the ATAPI signature is detected, do not send native ATA commands (READ SECTORS `0x20`, etc.) — they will fail. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"atapi: device signature detection"`. Add notes directly in this TODO section.

- [ ] On each ATA/AHCI port during enumeration:
  - [ ] Read task file registers: Sector Count, LBA Low, LBA Mid, LBA High
  - [ ] ATA signature: `SC=0x01, LBALo=0x01, LBAMid=0x00, LBAHi=0x00` → magnetic disk
  - [ ] ATAPI signature: `SC=0x01, LBALo=0x01, LBAMid=0x14, LBAHi=0xEB` → optical/tape
- [ ] Under AHCI: read `PxSIG` register — value `0xEB140101` = ATAPI device
- [ ] If ATAPI detected: set `port->device_type = DEVICE_ATAPI`
- [ ] If ATAPI detected: suppress all native ATA commands for this port
- [ ] Handle no-device case: `PxSIG = 0xFFFFFFFF` or status register reads `0xFF`
- [ ] Log: `[ATAPI] Port %d: ATAPI device detected (signature 0x14/0xEB)`
- [ ] Commit: `"atapi: device signature detection"`

### 1.2 IDENTIFY PACKET DEVICE (`0xA1`)

**Prompt:** Once the ATAPI signature is confirmed, issue IDENTIFY PACKET DEVICE (`0xA1`) — **never** IDENTIFY DEVICE (`0xEC`), which will abort on ATAPI hardware. This returns a 512-byte (256-word) data structure via PIO. Parse: Word 0 for protocol type (bits 15–14 = `10b`), SCSI device type (bits 12–8: `0x05` = CD/DVD), DRQ timing (bits 6–5), and command packet size (bits 1–0: `00b` = 12 bytes, `01b` = 16 bytes). Extract serial number (words 10–19, byte-swapped), model name (words 27–46, byte-swapped), and DMA capabilities (words 63, 88). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"atapi: IDENTIFY PACKET DEVICE parsing"`. Add notes directly in this TODO section.

- [ ] Issue `0xA1` (IDENTIFY PACKET DEVICE) — not `0xEC`
- [ ] Read 256 words (512 bytes) from data port after DRQ asserts
- [ ] Parse Word 0 — General Configuration:
  - [ ] Bits 15–14: confirm `10b` (ATAPI protocol)
  - [ ] Bits 12–8: SCSI peripheral type (`0x05` = CD/DVD-ROM, `0x00` = direct-access, `0x01` = tape)
  - [ ] Bits 6–5: DRQ timing (accelerated vs. delayed)
  - [ ] Bits 1–0: command packet size (`00b` = 12-byte, `01b` = 16-byte)
- [ ] Parse Words 10–19: serial number (20 chars, byte-swap each pair)
- [ ] Parse Words 27–46: model number (40 chars, byte-swap each pair)
- [ ] Parse Word 49: capabilities (bit 8 = LBA, bit 11 = IORDY)
- [ ] Parse Word 63: Multiword DMA modes (bits 2–0 = supported modes)
- [ ] Parse Word 88: Ultra DMA modes (bits 6–0 = supported modes)
- [ ] Store results in `atapi_device_t` struct
- [ ] Select highest supported DMA mode and program controller
- [ ] Register device: `blkdev_register("cdrom0", model, capacity)`
- [ ] Log: `[ATAPI] Device: %s, type=0x%02x, packet_size=%d, DMA=%s`
- [ ] Commit: `"atapi: IDENTIFY PACKET DEVICE parsing"`

---

## 2. ATAPI Packet Protocol (PIO)

### 2.1 PIO Packet State Machine

**Prompt:** Implement the full 8-step ATAPI PIO state machine for sending SCSI CDBs and receiving data. This is the foundational transport before DMA is available. The sequence is: (1) Select drive via Drive/Head register + 400ns delay, (2) Poll until BSY=0 and DRQ=0, (3) Write Features=0x00 (PIO mode), set byte count limit in LBA Mid/High, (4) Write 0xA0 to Command Register, (5) Wait for DRQ=1 with C/D=1 and I/O=0, (6) Write 12-byte (or 16-byte) SCSI CDB as 16-bit words, (7) Read data in chunks per interrupt/DRQ cycle, (8) Check status for errors. The 400ns delay after Drive/Head write is critical — typically achieved by reading Alternate Status 4 times. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"atapi: PIO packet state machine"`. Add notes directly in this TODO section.

- [ ] Implement `atapi_pio_command(port, cdb, cdb_len, buf, buf_len, direction)`:
  - [ ] Step 1: Write Drive/Head register (select Master/Slave), wait 400ns
  - [ ] Step 2: Poll Status until `(status & (BSY | DRQ)) == 0`
  - [ ] Step 3: Write Features register = `0x00` (PIO mode)
  - [ ] Step 4: Write byte count limit: LBA Mid = `buf_len & 0xFF`, LBA High = `(buf_len >> 8) & 0xFF`
  - [ ] Step 5: Write `0xA0` to Command Register
  - [ ] Step 6: Wait for DRQ=1, verify C/D=1 and I/O=0 in Sector Count register
  - [ ] Step 7: Write CDB to Data Register as `cdb_len / 2` 16-bit words
  - [ ] Step 8: Data phase: read 16-bit words from Data Register while DRQ is set
- [ ] Handle multi-block transfers: device may interrupt multiple times per block
- [ ] Read actual transfer size from LBA Mid/High after each DRQ assertion
- [ ] Check ERR bit in Status Register after completion → trigger REQUEST SENSE
- [ ] Implement 400ns delay: `inb(alt_status_port)` × 4
- [ ] Timeout: if BSY stays asserted > 30 seconds (optical spin-up), abort
- [ ] Pad all CDBs to device's expected packet size (typically 12 bytes, zero-pad)
- [ ] Commit: `"atapi: PIO packet state machine"`

---

## 3. ATAPI DMA Transport

### 3.1 Bus Master DMA for ATAPI

**Prompt:** PIO monopolizes the CPU while waiting for the slow optical drive. Implement Bus Master DMA for ATAPI: the only difference from PIO is writing `0x01` to the Features register (DMA mode) and using a Physical Region Descriptor Table (PRDT) for data transfer. Build the PRDT in contiguous physical memory: each 8-byte entry has a 32-bit physical address, 16-bit byte count, and EOT bit on the last entry. After writing the CDB, start the DMA engine via the Bus Master Command Register. The transfer completes asynchronously via interrupt. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"atapi: bus master DMA transfer"`. Add notes directly in this TODO section.

- [ ] Implement `atapi_dma_command(port, cdb, cdb_len, buf, buf_len, direction)`:
  - [ ] Allocate PRDT via `pmm_alloc_contiguous()` — array of 8-byte descriptors
  - [ ] Populate PRDT entries: `{ phys_addr, byte_count, flags }`, set EOT on last
  - [ ] Write PRDT physical address to Bus Master PRDT Address Register
  - [ ] Write Features register = `0x01` (DMA mode, not `0x00`)
  - [ ] Set byte count limit in LBA Mid/High (same as PIO)
  - [ ] Issue `0xA0` PACKET command, write CDB
  - [ ] Set Start bit in Bus Master Command Register
  - [ ] Return — transfer proceeds asynchronously
- [ ] DMA completion ISR:
  - [ ] Read Bus Master Status Register: check for error, interrupt, active bits
  - [ ] Clear interrupt bit by writing 1 to it
  - [ ] Clear Start bit to stop DMA engine
  - [ ] Read ATA Status Register to clear IRQ
  - [ ] Wake waiting thread via `event_set()`
- [ ] PRDT constraints: each entry ≤ 64 KB, must not cross 64 KB physical boundary
- [ ] Commit: `"atapi: bus master DMA transfer"`

### 3.2 AHCI ATAPI Command Delivery

**Prompt:** Under AHCI, the HBA automates the entire ATAPI handshake in silicon. The driver builds memory structures and rings a doorbell — no PIO polling required. Configure the Command Header: set CFL=5 (H2D FIS length in DWORDs), set the `a` bit (bit 5) to signal ATAPI mode, set `w` for write direction, set PRDTL for scatter-gather entries. In the Command Table: build FIS_REG_H2D (`0x27`) in the CFIS field with command=`0xA0`, place the raw SCSI CDB in the 16-byte ACMD field. Populate PRDT entries, write PxCI to issue. Completion arrives via MSI. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"atapi: AHCI command delivery"`. Add notes directly in this TODO section.

- [ ] Implement `ahci_atapi_command(port, cdb, cdb_len, buf, buf_len, direction)`:
  - [ ] Find free Command List slot (check PxCI and PxSACT)
  - [ ] Configure `HBA_CMD_HEADER`:
    - [ ] `CFL = 5` (Host-to-Device Register FIS = 5 DWORDs = 20 bytes)
    - [ ] `A = 1` (ATAPI — tells HBA to use packet protocol)
    - [ ] `W = (direction == WRITE) ? 1 : 0`
    - [ ] `PRDTL = num_prdt_entries`
  - [ ] Build Command Table (`HBA_CMD_TBL`), aligned to 128 bytes:
    - [ ] **CFIS** (64 bytes): construct `FIS_REG_H2D` (type `0x27`):
      - [ ] `fis_type = 0x27`, `command = 0xA0`, `c = 1` (command register update)
      - [ ] `featurel = 0x01` (DMA) or `0x00` (PIO)
      - [ ] `lba_mid = buf_len & 0xFF`, `lba_hi = (buf_len >> 8) & 0xFF`
    - [ ] **ACMD** (16 bytes): copy raw SCSI CDB (12 or 16 bytes, zero-pad remainder)
    - [ ] **PRDT**: populate entries with `{ dba, dbc, i_bit }`, max 4 MB per entry
  - [ ] Issue: write `(1 << slot)` to `PxCI`
- [ ] Completion: MSI fires, ISR reads `PxIS`, process `PxCI` clearance
- [ ] Error: check `PxTFD` for ERR bit → issue REQUEST SENSE
- [ ] Commit: `"atapi: AHCI command delivery"`

---

## 4. Transport Abstraction Layer

### 4.1 Unified SCSI Transport Interface

**Prompt:** The ATAPI SCSI logic must be completely decoupled from the physical transport. Create a `scsi_transport_t` interface with a single `send_command()` callback. Legacy IDE PIO, Legacy IDE DMA, and AHCI each implement this interface. The ATAPI device object calls `transport->send_command(cdb, data, len, direction)` without knowing or caring which hardware path is active. This guarantees the same SCSI READ(10) code works on a vintage 40-pin PATA cable and a modern SATA-III port. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"atapi: transport abstraction layer"`. Add notes directly in this TODO section.

- [ ] Define `scsi_transport_ops_t`:
  ```c
  typedef struct scsi_transport_ops {
      int (*send_command)(void *ctx, const uint8_t *cdb, size_t cdb_len,
                          void *data, size_t data_len, int direction);
  } scsi_transport_ops_t;
  ```
- [ ] Implement `ide_pio_transport`: calls `atapi_pio_command()`
- [ ] Implement `ide_dma_transport`: calls `atapi_dma_command()`
- [ ] Implement `ahci_transport`: calls `ahci_atapi_command()`
- [ ] `atapi_device_t` holds a `scsi_transport_ops_t *transport` pointer
- [ ] All SCSI MMC commands use `dev->transport->send_command()` exclusively
- [ ] Auto-detect transport at init: prefer AHCI > DMA > PIO
- [ ] Log: `[ATAPI] Using transport: %s`
- [ ] Commit: `"atapi: transport abstraction layer"`

---

## 5. Core SCSI MMC Commands

### 5.1 TEST UNIT READY & INQUIRY

**Prompt:** Implement the two most fundamental SCSI commands. TEST UNIT READY (`0x00`) is a no-data command that checks if the device is ready (media present, spun up). It must be sent repeatedly during spin-up, with delays between retries. INQUIRY (`0x12`) retrieves 36 bytes of device identification: peripheral type, vendor name, product name, revision. Both commands must be padded to the device's CDB size (12 bytes). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"atapi: TEST UNIT READY and INQUIRY"`. Add notes directly in this TODO section.

- [ ] Implement `atapi_test_unit_ready(dev)`:
  - [ ] CDB: `{ 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }` (12 bytes, zero-padded)
  - [ ] No data transfer — check status only
  - [ ] If CHECK CONDITION → issue REQUEST SENSE to determine cause
  - [ ] Retry loop: up to 10 retries with 500ms delay (drive may be spinning up)
  - [ ] Return: 0 = ready, -ENOMEDIUM = no disc, -EAGAIN = spinning up
- [ ] Implement `atapi_inquiry(dev, buf)`:
  - [ ] CDB: `{ 0x12, 0x00, 0x00, 0x00, 0x24, 0x00, ... }` (allocation length = 36)
  - [ ] Parse 36-byte response:
    - [ ] Byte 0 bits 4–0: peripheral device type (`0x05` = CD/DVD)
    - [ ] Bytes 8–15: vendor identification (8 bytes, space-padded ASCII)
    - [ ] Bytes 16–31: product identification (16 bytes)
    - [ ] Bytes 32–35: product revision (4 bytes)
  - [ ] Store in `dev->vendor`, `dev->product`, `dev->revision`
- [ ] Log: `[ATAPI] INQUIRY: %s %s rev %s`
- [ ] Commit: `"atapi: TEST UNIT READY and INQUIRY"`

### 5.2 READ CAPACITY & READ (10)

**Prompt:** READ CAPACITY (`0x25`) returns the last LBA and block size (typically 2048 for data CDs). Both fields are 32-bit Big-Endian — they MUST be byte-swapped on x86. Total capacity = `(Last_LBA + 1) × Block_Size`. READ (10) (`0x28`) reads data blocks. The transfer length field is block count, not byte count. For a 2048-byte-sector CD, reading 512 blocks reads 1 MB. LBA and transfer length are Big-Endian in the CDB. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"atapi: READ CAPACITY and READ(10)"`. Add notes directly in this TODO section.

- [ ] Implement `atapi_read_capacity(dev, *last_lba, *block_size)`:
  - [ ] CDB: `{ 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }`
  - [ ] Read 8-byte response
  - [ ] Byte-swap bytes 0–3 → `last_lba` (Big-Endian to Little-Endian)
  - [ ] Byte-swap bytes 4–7 → `block_size`
  - [ ] Validate: `block_size` should be 2048 (data CD/DVD) or 2352 (raw)
  - [ ] Compute: `capacity = (last_lba + 1) * block_size`
  - [ ] Store in `dev->capacity`, `dev->block_size`
- [ ] Implement `atapi_read(dev, lba, block_count, buf)`:
  - [ ] CDB: `{ 0x28, 0x00, LBA[3], LBA[2], LBA[1], LBA[0], 0x00, Len[1], Len[0], 0x00, 0x00, 0x00 }`
  - [ ] LBA and Len encoded Big-Endian in CDB
  - [ ] Buffer size = `block_count * dev->block_size`
  - [ ] For requests > 65535 blocks: split into multiple READ(10) commands
  - [ ] Allocate read buffer via `pmm_alloc_contiguous()` for DMA
- [ ] Log: `[ATAPI] Capacity: %u MB (%u blocks × %u bytes)`
- [ ] Commit: `"atapi: READ CAPACITY and READ(10)"`

### 5.3 Media Control Commands

**Prompt:** Implement the optical media control commands: START STOP UNIT (`0x1B`) for spin-up/spin-down/eject/load, PREVENT ALLOW MEDIUM REMOVAL (`0x1E`) for tray locking, and READ TOC (`0x43`) for extracting the Table of Contents. START STOP UNIT uses the LOEJ bit (bit 1) and Start bit (bit 0): LOEJ=1+Start=0 = eject, LOEJ=1+Start=1 = load (close tray), LOEJ=0+Start=1 = spin up. READ TOC returns track/session information needed for multi-session CD and audio CD support. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"atapi: media control commands"`. Add notes directly in this TODO section.

- [ ] Implement `atapi_eject(dev)`:
  - [ ] CDB: `{ 0x1B, 0, 0, 0, 0x02, 0, ... }` (LOEJ=1, Start=0 = eject)
- [ ] Implement `atapi_load(dev)`:
  - [ ] CDB: `{ 0x1B, 0, 0, 0, 0x03, 0, ... }` (LOEJ=1, Start=1 = close tray)
- [ ] Implement `atapi_spin_up(dev)`:
  - [ ] CDB: `{ 0x1B, 0, 0, 0, 0x01, 0, ... }` (LOEJ=0, Start=1 = spin up)
- [ ] Implement `atapi_lock_tray(dev, bool lock)`:
  - [ ] CDB: `{ 0x1E, 0, 0, 0, lock ? 0x01 : 0x00, 0, ... }`
- [ ] Implement `atapi_read_toc(dev, buf, buf_len, format)`:
  - [ ] CDB: `{ 0x43, (msf << 1), format, 0, 0, 0, track, Len[1], Len[0], 0, 0, 0 }`
  - [ ] Format 0: TOC — returns track list with start LBAs
  - [ ] Format 1: Multi-session info — returns last session start LBA
  - [ ] Parse TOC header: data length (2 bytes BE), first track, last track
  - [ ] Parse track descriptors: track number, ADR/Control, start LBA (4 bytes BE)
  - [ ] Byte-swap all multi-byte fields
- [ ] Implement `atapi_read_disc_structure(dev, media_type, layer, format, buf)`:
  - [ ] CDB: `{ 0xAD, media_type, 0, 0, layer, 0, 0, format, Len[1], Len[0], 0, 0 }`
  - [ ] Format 0x00: Physical Format Info (DVD layer info: track path, density, sectors)
  - [ ] Format 0x01: DVD Copyright (CSS/CPRM protected status)
  - [ ] Format 0x04: DVD Manufacturing Info
  - [ ] Byte-swap all BE response fields
- [ ] Wire eject to device manager / Explorer context menu
- [ ] Commit: `"atapi: media control commands"`

### 5.4 GET CONFIGURATION & Feature Profiles

**Prompt:** GET CONFIGURATION (`0x46`) retrieves the drive's capability profile list and active features. This is essential for determining what the drive can do: read CD, read DVD, read BD, write CD-R, write DVD±R/RW, etc. Each profile has a numeric code (e.g., `0x0008` = CD-ROM, `0x0010` = DVD-ROM, `0x0040` = BD-ROM). The current profile indicates what type of media is currently inserted. Feature descriptors provide fine-grained capabilities: core features, morphing, removable medium, random readable, multi-read, CD read, DVD read, BD read, power management, etc. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"atapi: GET CONFIGURATION and feature profiles"`. Add notes directly in this TODO section.

- [ ] Implement `atapi_get_configuration(dev, rt, start_feature, buf, buf_len)`:
  - [ ] CDB: `{ 0x46, rt, Start[1], Start[0], 0, 0, 0, Len[1], Len[0], 0, 0, 0 }`
  - [ ] RT=0x00: all features, RT=0x01: current features, RT=0x02: single feature
- [ ] Parse Feature Header (8 bytes):
  - [ ] Data length (4 bytes BE)
  - [ ] Current profile (2 bytes BE) — what media type is inserted
- [ ] Parse Profile List (Feature Code `0x0000`):
  - [ ] Each profile descriptor: 4 bytes (profile number BE + current bit)
  - [ ] Key profiles:
    - [ ] `0x0008` CD-ROM, `0x0009` CD-R, `0x000A` CD-RW
    - [ ] `0x0010` DVD-ROM, `0x0011` DVD-R, `0x001A` DVD+RW, `0x001B` DVD+R
    - [ ] `0x0040` BD-ROM, `0x0041` BD-R, `0x0043` BD-RE
- [ ] Detect drive type from profile list (CD-only, DVD combo, BD combo)
- [ ] Detect current media type from current profile
- [ ] Store capabilities in `dev->profiles[]` and `dev->current_profile`
- [ ] Log: `[ATAPI] Drive supports: %s, current media: %s`
- [ ] Commit: `"atapi: GET CONFIGURATION and feature profiles"`

### 5.5 MODE SENSE & Drive Capabilities

**Prompt:** MODE SENSE (10) (`0x5A`) retrieves drive operating parameters organized into mode pages. Critical pages for optical drives: Page `0x01` (Error Recovery — retry counts, error reporting), Page `0x2A` (Capabilities and Mechanical Status — read/write speeds, buffer size, media types supported, tray/caddy/slot mechanism type, audio capabilities). Page `0x2A` is particularly important for determining the drive's maximum read/write speeds and supported features. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"atapi: MODE SENSE and drive capabilities"`. Add notes directly in this TODO section.

- [ ] Implement `atapi_mode_sense(dev, page_code, buf, buf_len)`:
  - [ ] CDB: `{ 0x5A, 0x00, page_code, 0, 0, 0, 0, Len[1], Len[0], 0, 0, 0 }`
  - [ ] Use MODE SENSE (10) — not MODE SENSE (6), which some drives don't support
- [ ] Parse Mode Parameter Header (8 bytes):
  - [ ] Mode Data Length (2 bytes BE), Medium Type, Device Specific Parameter
- [ ] Parse Page `0x01` — Read/Write Error Recovery:
  - [ ] Error recovery flags, read retry count, write retry count
- [ ] Parse Page `0x2A` — Capabilities and Mechanical Status:
  - [ ] Read capabilities: CD-R read, CD-RW read, DVD-ROM read, DVD-R read, etc.
  - [ ] Write capabilities: CD-R write, CD-RW write, DVD-R write, etc.
  - [ ] Mechanism type: tray (0), caddy (1), popup (2), changer (4), slot (5)
  - [ ] Maximum read speed (KB/s), current read speed
  - [ ] Maximum write speed (KB/s), current write speed
  - [ ] Buffer size (KB)
  - [ ] Audio capabilities: digital audio output, accurate DAE stream, C2 error pointers
- [ ] Store in `dev->capabilities`
- [ ] Log: `[ATAPI] Capabilities: %s, max_read=%u KB/s, buf=%u KB, mech=%s`
- [ ] Commit: `"atapi: MODE SENSE and drive capabilities"`

---

## 6. Error Handling & Sense Data

### 6.1 REQUEST SENSE & Sense Key Parsing

**Prompt:** When any ATAPI command fails (ERR bit set in ATA Status Register), the device caches detailed SCSI error information internally. The driver must issue REQUEST SENSE (`0x03`) to retrieve 18 bytes of fixed-format sense data. Parse three critical fields: Sense Key (byte 2, bits 3–0), ASC (byte 12), and ASCQ (byte 13). Map the Sense Key / ASC / ASCQ triple to a meaningful kernel error code. Key mappings: `0x02/0x3A/0x00` = no medium, `0x02/0x04/0x01` = becoming ready (spinning up), `0x06/0x28/0x00` = media changed (cache invalidation required), `0x05/0x20/0x00` = invalid command. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"atapi: REQUEST SENSE and error handling"`. Add notes directly in this TODO section.

- [ ] Implement `atapi_request_sense(dev, sense_buf)`:
  - [ ] CDB: `{ 0x03, 0x00, 0x00, 0x00, 0x12, 0x00, ... }` (allocation length = 18)
  - [ ] Read 18-byte fixed-format sense data
- [ ] Parse sense data:
  - [ ] Byte 0 bits 6–0: response code (`0x70` = current, `0x71` = deferred)
  - [ ] Byte 2 bits 3–0: Sense Key
  - [ ] Byte 12: Additional Sense Code (ASC)
  - [ ] Byte 13: Additional Sense Code Qualifier (ASCQ)
- [ ] Map Sense Key / ASC / ASCQ to kernel errors:
  - [ ] `0x00` No Sense → success (command completed)
  - [ ] `0x02 / 0x3A / 0x00` Not Ready, Medium Not Present → `-ENOMEDIUM`
  - [ ] `0x02 / 0x04 / 0x01` Not Ready, Becoming Ready → `-EAGAIN` (retry after delay)
  - [ ] `0x03` Medium Error → `-EIO` (scratched/unreadable media)
  - [ ] `0x04` Hardware Error → `-EIO` (internal drive failure)
  - [ ] `0x05 / 0x20 / 0x00` Illegal Request, Invalid Opcode → `-ENOSYS`
  - [ ] `0x05 / 0x24 / 0x00` Illegal Request, Invalid Field → `-EINVAL`
  - [ ] `0x06 / 0x28 / 0x00` Unit Attention, Media Changed → `-EMEDIUMTYPE` + cache invalidate
  - [ ] `0x06 / 0x29 / 0x00` Unit Attention, Power On/Reset → re-initialize
  - [ ] `0x0B` Aborted Command → retry
- [ ] Auto-retry on transient errors (Sense Key 0x02 with ASC 0x04): up to 10 retries
- [ ] Log: `[ATAPI] Sense: key=0x%02x ASC=0x%02x ASCQ=0x%02x (%s)`
- [ ] Commit: `"atapi: REQUEST SENSE and error handling"`

### 6.2 Media Change Detection & Cache Invalidation

**Prompt:** When a user swaps discs, the drive signals Unit Attention (Sense Key `0x06`, ASC `0x28`). The kernel must detect this on every command failure and invalidate all cached data: filesystem caches, sector caches, mounted filesystem state. Also implement proactive media change polling via GET EVENT STATUS NOTIFICATION (`0x4A`) in polled mode — this allows detecting disc swaps without waiting for a failed read. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"atapi: media change detection"`. Add notes directly in this TODO section.

- [ ] On Sense Key `0x06` / ASC `0x28` (Media Changed):
  - [ ] Invalidate all sector caches for this device
  - [ ] Unmount any mounted filesystem on this device
  - [ ] Re-read capacity (media may have changed size/type)
  - [ ] Notify VFS: `vfs_media_changed(dev)`
  - [ ] Notify Disk Manager GUI: media removal event
- [ ] Implement `atapi_poll_media_change(dev)`:
  - [ ] CDB: `{ 0x4A, 0x01, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00 }`
  - [ ] Polled mode (bit 0 of byte 1 = 1)
  - [ ] Parse event header: NEA (No Event Available), notification class, supported events
  - [ ] Media event class = `0x04`: check for media removal, insertion, eject request
- [ ] Periodic polling: timer callback every 2 seconds when no I/O active
- [ ] On media insertion: auto-detect filesystem (ISO 9660 / UDF) and offer auto-mount
- [ ] Log: `[ATAPI] Media change detected — invalidating caches`
- [ ] Commit: `"atapi: media change detection"`

---

## 7. Block Device Integration

### 7.1 VFS Block Device Registration

**Prompt:** Register the ATAPI device as a read-only block device with the VFS layer. The block size is the media's native sector size (2048 for data CDs/DVDs). Expose standard block device operations: read, ioctl (eject, lock, read TOC). Write operations should return `-EROFS` for read-only media (most optical drives). For CD-RW/DVD-RW, detect write capability from the device type and feature profiles. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"atapi: VFS block device registration"`. Add notes directly in this TODO section.

- [ ] Implement `atapi_blkdev_ops`:
  - [ ] `read(dev, lba, count, buf)` → `atapi_read()` with 2048-byte sectors
  - [ ] `write(dev, lba, count, buf)` → `-EROFS` for read-only media
  - [ ] `ioctl(dev, cmd, arg)`:
    - [ ] `CDROM_EJECT` → `atapi_eject()`
    - [ ] `CDROM_CLOSE_TRAY` → `atapi_load()`
    - [ ] `CDROM_LOCK_DOOR` → `atapi_lock_tray(true)`
    - [ ] `CDROM_UNLOCK_DOOR` → `atapi_lock_tray(false)`
    - [ ] `CDROM_READ_TOC` → `atapi_read_toc()`
    - [ ] `CDROM_GET_CAPACITY` → `atapi_read_capacity()`
  - [ ] `flush(dev)` → no-op for read-only media
- [ ] Register with block device layer: `blkdev_register("cdrom0", ...)`
- [ ] Assign drive letter: `D:\` (or next available)
- [ ] Register with Device Manager: "CD-ROM Drive" category
- [ ] Log: `[ATAPI] Block device cdrom0 registered (capacity=%u MB, block_size=%u)`
- [ ] Commit: `"atapi: VFS block device registration"`

---

## 8. Optical Filesystem Support

### 8.1 ISO 9660 / Joliet Filesystem Driver

**Prompt:** ISO 9660 (ECMA-119) is the standard CD-ROM filesystem. The Primary Volume Descriptor (PVD) is at LBA 16 (sector 16 × 2048 bytes). It contains the root directory record, volume name, creation date, and block size. Implement a read-only ISO 9660 driver: mount via PVD detection (type code `0x01`, identifier `"CD001"`), parse directory records (variable-length, with interleaving), handle 8.3 filenames. Add Joliet extension support: look for Supplementary Volume Descriptor (type `0x02`) with escape sequences `%/@`, `%/C`, or `%/E` — these indicate UCS-2 Unicode filenames up to 64 characters. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"iso9660: filesystem driver with Joliet"`. Add notes directly in this TODO section.

- [ ] Implement `iso9660_mount(blkdev)`:
  - [ ] Read LBA 16 (offset 32768 bytes) for Volume Descriptor Set
  - [ ] Scan for Primary Volume Descriptor: type=`0x01`, magic=`"CD001"`, version=`0x01`
  - [ ] Parse PVD fields:
    - [ ] Volume identifier (32 bytes)
    - [ ] Logical block size (2048 typically)
    - [ ] Volume size in blocks (both-endian — use LE copy at offset 80)
    - [ ] Root directory record (34 bytes at offset 156): LBA, data length
    - [ ] Path table LBA and size
  - [ ] Scan for Supplementary Volume Descriptor (type=`0x02`) for Joliet:
    - [ ] Check escape sequences at offset 88: `%/@`, `%/C`, `%/E`
    - [ ] If found: prefer Joliet root directory and filenames (UCS-2)
  - [ ] Scan for Volume Descriptor Set Terminator (type=`0xFF`)
- [ ] Implement directory parsing:
  - [ ] Read directory extent from root directory LBA
  - [ ] Parse variable-length directory records:
    - [ ] Byte 0: record length (0 = padding to sector boundary)
    - [ ] Bytes 2–9: LBA of file data (both-endian, use LE at offset 2)
    - [ ] Bytes 10–17: data length (both-endian)
    - [ ] Byte 25: file flags (bit 1 = directory)
    - [ ] Byte 32: filename length, followed by filename
    - [ ] Handle `;1` version suffix (strip for display)
    - [ ] Handle `.` (self) and `..` (parent) records
  - [ ] For Joliet: decode UCS-2 filenames to UTF-8
- [ ] Implement `iso9660_read_file(inode, offset, buf, len)`
- [ ] Wire to VFS: register `iso9660_ops` with file/dir callbacks
- [ ] Auto-detect: try `iso9660_mount()` on any newly inserted CD/DVD
- [ ] Commit: `"iso9660: filesystem driver with Joliet"`

### 8.2 UDF Filesystem Support (Read-Only)

**Prompt:** UDF (Universal Disk Format, ECMA-167 / ISO 13346) is required for DVD-ROM, Blu-ray, and packet-written CD-RW media. UDF uses different on-disc structures: Anchor Volume Descriptor Pointer (AVDP) at LBA 256, Partition Descriptors, Logical Volume Descriptors, and File Entry / File Identifier Descriptor structures. Implement a basic read-only UDF driver: detect AVDP, parse partition and logical volume descriptors, navigate the File Entry tree, and read file data from allocation descriptors. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"udf: basic read-only filesystem driver"`. Add notes directly in this TODO section.

- [ ] Implement `udf_mount(blkdev)`:
  - [ ] Read LBA 256 for Anchor Volume Descriptor Pointer (AVDP)
    - [ ] Validate descriptor tag: tag ID = `0x0002`
    - [ ] Extract Main VDS extent (location + length) and Reserve VDS extent
  - [ ] Read Main Volume Descriptor Sequence:
    - [ ] Parse Primary Volume Descriptor (tag ID `0x0001`)
    - [ ] Parse Partition Descriptor (tag ID `0x0005`): partition start LBA, length
    - [ ] Parse Logical Volume Descriptor (tag ID `0x0006`): partition map, FSD location
  - [ ] Read File Set Descriptor from logical volume
  - [ ] Locate root ICB (Information Control Block) from FSD
- [ ] Implement File Entry / Extended File Entry parsing:
  - [ ] Parse allocation descriptors (short, long, or extended)
  - [ ] Support inline data (embedded in ICB)
  - [ ] Read file data from allocation extents
- [ ] Implement File Identifier Descriptor parsing for directories:
  - [ ] Extract filename (d-string format: length byte + UTF-8/UTF-16 data)
  - [ ] Handle parent directory entry (`..\` equivalent)
- [ ] Wire to VFS with read-only operations
- [ ] Auto-detect: try UDF after ISO 9660 fails (some DVDs are UDF-only)
- [ ] Log: `[UDF] Mounted: partition at LBA %u, %u MB`
- [ ] Commit: `"udf: basic read-only filesystem driver"`

---

## 9. Audio CD Support (🚀 Impossible OS Feature)

### 9.1 CD Digital Audio Extraction

**Prompt:** Implement audio CD support using READ CD (`0xBE`), which can read raw 2352-byte audio sectors. Detect audio CDs via READ TOC (tracks with control field bit 2 = 0 are audio). Extract digital audio data (16-bit stereo PCM, 44.1 kHz, Little-Endian) for playback or ripping. Support PLAY AUDIO (10) (`0x45`) and PAUSE/RESUME (`0x4B`) for hardware-driven playback. Read SUB-CHANNEL (`0x42`) for playback position tracking. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"atapi: audio CD extraction and playback"`. Add notes directly in this TODO section.

- [ ] Detect audio CD: `atapi_read_toc()` → check Control field per track (bit 2 = 0 → audio)
- [ ] Implement `atapi_read_cd_audio(dev, lba, block_count, buf)`:
  - [ ] CDB: `{ 0xBE, 0x00, LBA[3..0], Len[2..0], 0x10, 0x00, 0x00, 0x00 }`
  - [ ] Expected sector type = `0x00` (any), subchannel = `0x00`
  - [ ] Read raw 2352-byte sectors (no error correction header)
  - [ ] Data format: 16-bit signed PCM, stereo, 44.1 kHz, Little-Endian
- [ ] Implement `atapi_play_audio(dev, start_lba, end_lba)`:
  - [ ] CDB: `{ 0x45, 0, Start[3..0], End[3..0], 0, 0 }`
- [ ] Implement `atapi_pause_resume(dev, bool resume)`:
  - [ ] CDB: `{ 0x4B, 0, 0, 0, 0, 0, 0, 0, resume ? 0x01 : 0x00, 0, 0, 0 }`
- [ ] Implement `atapi_read_subchannel(dev, *position)`:
  - [ ] CDB: `{ 0x42, 0x02, 0x40, 0x01, 0, 0, 0, 0x00, 0x10, 0, 0, 0 }`
  - [ ] Parse: current position in MSF, playback status (playing/paused/stopped)
- [ ] SET CD SPEED (`0xBB`): set read speed for quieter operation or max throughput
- [ ] Wire to audio player application / CD Player applet
- [ ] Commit: `"atapi: audio CD extraction and playback"`

### 9.2 Paranoia-Grade DAE with C2 Error Pointers

**Prompt:** Standard audio extraction tolerates interpolated errors — the drive silently substitutes guessed samples for unreadable ones. Paranoia-grade extraction detects and corrects these errors for bit-perfect audio recovery from scratched discs. Check MODE SENSE Page `0x2A` for C2 Error Pointers support (Capability byte, bit 4) and Accurate Stream support (bit 1). When C2 is supported, READ CD (`0xBE`) can return a 294-byte C2 error pointer block alongside each 2352-byte audio sector — each bit indicates whether the corresponding byte is in error. When errors are detected, re-read the sector with jitter correction (overlapping reads with offset verification). No OS ships with a built-in paranoia-grade ripper as part of the operating system itself. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"atapi: paranoia-grade DAE with C2 error detection"`. Add notes directly in this TODO section.

> [!TIP]
> **Competitive Edge:** Windows relies on third-party rippers (EAC, dbpoweramp).
> Linux uses `cdparanoia` (separate package, not integrated). Neither OS has
> built-in bit-perfect audio extraction with C2 error reporting. Impossible OS
> can ship this as a native, first-class audio CD feature.

- [ ] Check capabilities via MODE SENSE Page `0x2A`:
  - [ ] Bit 4: C2 Error Pointers supported
  - [ ] Bit 1: Accurate Stream (DAE does not interpolate errors)
- [ ] If C2 supported, modify READ CD (`0xBE`) header byte:
  - [ ] Byte 9 bit 1: request C2 error block (294 bytes per sector)
  - [ ] Total sector size: 2352 (audio) + 294 (C2) = 2646 bytes
- [ ] Parse C2 error block:
  - [ ] 294 bytes × 8 bits = 2352 bits → one bit per audio sample byte
  - [ ] Bit = 1 → corresponding byte is in error
  - [ ] Count total C2 errors per sector
- [ ] On C2 errors detected:
  - [ ] Re-read sector up to 8 times
  - [ ] Use overlapping reads (read sector N ± 1) to verify alignment
  - [ ] If all retries have errors → mark sector as uncorrectable, log warning
- [ ] Expose via API: `cdrom_rip_track(dev, track, buf, *quality_report)`
- [ ] Quality report per track: total sectors, error-free sectors, retried sectors, uncorrectable
- [ ] Wire to CD Player applet: "Rip Track" with quality meter
- [ ] Commit: `"atapi: paranoia-grade DAE with C2 error detection"`

---

## 10. Asynchronous I/O & Concurrency

### 10.1 Interrupt-Driven Async ATAPI

**Prompt:** Optical drives have extremely high latency (seconds for spin-up, hundreds of milliseconds for seek). The ATAPI driver must never block the kernel during I/O. Implement fully asynchronous command submission: submit SCSI CDB, return immediately, wake caller via event when ISR fires on completion. For AHCI, this is native (MSI on PxCI clearance). For legacy IDE, register IRQ14/IRQ15 handlers. Use per-device mutexes to prevent concurrent Task File/Command Table corruption. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"atapi: async I/O and concurrency"`. Add notes directly in this TODO section.

- [ ] Add `event_t command_complete` to `atapi_device_t`
- [ ] Add `mutex_t port_lock` to prevent concurrent access
- [ ] Legacy IDE:
  - [ ] Register IRQ handler for IRQ14 (primary) / IRQ15 (secondary) via IOAPIC
  - [ ] ISR: read Status Register to acknowledge, call `event_set()`
- [ ] AHCI:
  - [ ] ISR reads `PxIS`, checks for D2H Register FIS / Task File Error
  - [ ] Clear interrupt bits, call `event_set()`
- [ ] Submission path: `mutex_lock()`, submit command, `event_wait(timeout_ms)`
- [ ] Timeout: 30 seconds for spin-up, 10 seconds for normal I/O
- [ ] On timeout: log error, attempt device reset (write SRST or COMRESET)
- [ ] Pre-scheduler fallback: if interrupts not available, use PIO polling
- [ ] Commit: `"atapi: async I/O and concurrency"`

---

## 11. Drive Noise Control & Speed Management (🚀 Impossible OS Feature)

### 11.1 SET CD SPEED & Quiet Mode

**Prompt:** Optical drives at maximum speed are extremely loud (40x+ CD = jet turbine). Implement SET CD SPEED (`0xBB`) to allow the user to select between maximum performance and quiet operation. Parse the drive's current speed from MODE SENSE Page `0x2A` (maximum read/write speeds). Offer preset modes: "Performance" (max speed), "Quiet" (8x CD / 4x DVD), "Silent" (4x CD / 2x DVD). Expose via Drive Properties dialog: a speed slider or dropdown. This is a quality-of-life feature no OS provides natively in the GUI. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"atapi: drive speed and noise control"`. Add notes directly in this TODO section.

> [!TIP]
> **Competitive Edge:** Neither Windows nor Linux provides a built-in GUI to
> control optical drive speed and noise. Users must install third-party tools
> (e.g., Nero DriveSpeed, hdparm). Impossible OS can offer this natively in
> the Drive Properties dialog — a significant usability win for laptop users.

- [ ] Implement `atapi_set_speed(dev, read_speed_kbps, write_speed_kbps)`:
  - [ ] CDB: `{ 0xBB, 0, Read[1], Read[0], Write[1], Write[0], 0, 0, 0, 0, 0, 0 }`
  - [ ] Values in KB/s (e.g., CD 1x = 176, DVD 1x = 1385, BD 1x = 4500)
  - [ ] `0xFFFF` = maximum speed (let drive decide)
- [ ] Speed presets:
  - [ ] Performance: `0xFFFF` (maximum)
  - [ ] Balanced: CD 24x (4224), DVD 8x (11080)
  - [ ] Quiet: CD 8x (1408), DVD 4x (5540)
  - [ ] Silent: CD 4x (704), DVD 2x (2770)
- [ ] Auto-quiet: reduce speed when laptop is on battery power
- [ ] Expose via Drive Properties: speed preset dropdown
- [ ] Expose via Registry: `HKLM\SYSTEM\Drivers\ATAPI\DriveSpeedPreset`
- [ ] Log: `[ATAPI] Speed set to %u KB/s (read), %u KB/s (write)`
- [ ] Commit: `"atapi: drive speed and noise control"`

---

## 12. Rock Ridge & El Torito (ISO 9660 Extensions)

### 12.1 Rock Ridge Extension Support

**Prompt:** Rock Ridge (IEEE P1282) extends ISO 9660 with POSIX-compatible metadata: long filenames (up to 255 chars without the 8.3 restriction), symbolic links, file permissions (owner/group/world), device nodes, and deep directory nesting beyond the ISO 9660 8-level limit. Rock Ridge data is stored in System Use Entries (SUEs) appended to each directory record's System Use area. Key entries: `PX` (POSIX attributes), `NM` (alternate/long name), `SL` (symbolic link), `CL`/`PL`/`RE` (relocated directories). Detect Rock Ridge by finding `SP` (RRIP Sharing Protocol) and `ER` (Extension Reference) entries at the root directory. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"iso9660: Rock Ridge extension support"`. Add notes directly in this TODO section.

- [ ] Detect Rock Ridge: scan root directory record's System Use area:
  - [ ] Look for `SP` entry (signature bytes `0xBE`, `0xEF`) → RRIP indicator
  - [ ] Look for `ER` entry → confirms "RRIP_1991A" or "IEEE_P1282" extension
- [ ] Parse System Use Entries (SUEs) for each directory record:
  - [ ] `NM` (Alternate Name): long filename, replaces 8.3 ISO name
    - [ ] Handle continuation (`CE` entry) for very long names
  - [ ] `PX` (POSIX Attributes): file mode, nlinks, uid, gid
  - [ ] `SL` (Symbolic Link): component records → target path
  - [ ] `CL` (Child Link): relocated directory LBA
  - [ ] `PL` (Parent Link): parent of relocated directory
  - [ ] `RE` (Relocated): marks entry as relocated (skip in normal listing)
  - [ ] `TF` (Timestamps): creation, modification, access, attributes times
- [ ] If Rock Ridge detected, prefer `NM` names over ISO 8.3 names
- [ ] Map POSIX permissions to VFS stat → support `chmod` semantics (read-only)
- [ ] Commit: `"iso9660: Rock Ridge extension support"`

### 12.2 El Torito Bootable CD/DVD

**Prompt:** El Torito (Phoenix/IBM, 1995) enables booting from CD/DVD-ROM. The Boot Record Volume Descriptor is at LBA 17 (one sector after the PVD). It contains a pointer to a Boot Catalog which lists bootable images (emulated floppy, hard disk, or no-emulation). Impossible OS installation media will be an El Torito bootable ISO with UEFI boot support. Detecting El Torito is needed to identify bootable CDs in the Disk Manager. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"iso9660: El Torito boot detection"`. Add notes directly in this TODO section.

- [ ] Detect Boot Record Volume Descriptor at LBA 17:
  - [ ] Type code = `0x00`, identifier = `"CD001"`, version = `0x01`
  - [ ] Boot System Identifier = `"EL TORITO SPECIFICATION"` (32 bytes)
  - [ ] Boot Catalog LBA at offset 0x47 (4 bytes LE)
- [ ] Parse Boot Catalog:
  - [ ] Validation Entry (32 bytes): Header ID `0x01`, platform ID, checksum
  - [ ] Default Entry (32 bytes): boot indicator, boot media type, load segment, LBA, sector count
  - [ ] Boot media types: 0=no emulation, 1=1.2M floppy, 2=1.44M floppy, 3=2.88M floppy, 4=hard disk
- [ ] Detect UEFI boot entries: platform ID = `0xEF` (EFI)
- [ ] Display in Disk Manager: "Bootable" badge on bootable optical media
- [ ] Commit: `"iso9660: El Torito boot detection"`

---

## Priority Order

| Priority | Section                            | Description                                              |
|----------|------------------------------------|----------------------------------------------------------|
| 🔴 P0    | 1.1 ATAPI Signature Detection    | Foundation — must identify optical drives before any command |
| 🔴 P0    | 1.2 IDENTIFY PACKET DEVICE       | Foundation — required to parse device capabilities       |
| 🔴 P0    | 2.1 PIO Packet State Machine     | Foundation — basic ATAPI communication                   |
| 🔴 P0    | 5.2 READ CAPACITY & READ(10)     | Foundation — read data from optical media                |
| 🟠 P1    | 6.1 REQUEST SENSE                | Correctness — all error handling depends on sense data   |
| 🟠 P1    | 5.1 TEST UNIT READY & INQUIRY    | Correctness — device probing and media presence check    |
| 🟠 P1    | 4.1 Transport Abstraction        | Architecture — decouple SCSI from transport hardware     |
| 🟠 P1    | 7.1 VFS Block Device             | Integration — expose optical drive to filesystem layer   |
| 🟠 P1    | 5.4 GET CONFIGURATION            | Foundation — determine drive type and media capabilities |
| 🟡 P2    | 3.2 AHCI ATAPI Delivery          | Performance — bypass PIO handshake via AHCI hardware     |
| 🟡 P2    | 3.1 Bus Master DMA               | Performance — async DMA instead of PIO polling           |
| 🟡 P2    | 5.3 Media Control Commands       | Feature — eject, load, lock tray, read TOC, disc struct  |
| 🟡 P2    | 5.5 MODE SENSE                   | Feature — drive capabilities, speeds, mechanism type     |
| 🟡 P2    | 6.2 Media Change Detection       | Correctness — handle disc swaps gracefully               |
| 🟡 P2    | 10.1 Async I/O & Concurrency     | Performance — non-blocking optical drive access          |
| 🟢 P3    | 8.1 ISO 9660 / Joliet            | Feature — standard CD/DVD filesystem support             |
| 🟢 P3    | 8.2 UDF Filesystem               | Feature — DVD-ROM and Blu-ray filesystem support         |
| 🟢 P3    | 12.1 Rock Ridge ⭐               | Feature — long filenames and POSIX attrs on ISO 9660     |
| 🟢 P3    | 12.2 El Torito                   | Feature — bootable CD/DVD detection and display          |
| 🔵 P4    | 9.1 Audio CD Extraction          | Feature — audio CD playback and digital extraction       |
| 🔵 P4    | 9.2 Paranoia DAE + C2 ⭐         | **Bit-perfect ripping** — no OS ships this built-in     |
| 🔵 P4    | 11.1 Drive Speed Control ⭐      | **Noise management** — no OS has a native GUI for this  |

> [!NOTE]
> ⭐ = Feature where Impossible OS can be **superior** to both Windows and Linux.

---

## OS Comparison

| Feature                            | 🪟 Windows 11 (cdrom.sys)            | 🐧 Linux (sr / ide-cd)               | 🚀 Impossible OS                               |
| ---------------------------------- | ------------------------------------- | -------------------------------------- | ----------------------------------------------- |
| ATAPI device detection             | ✅ PnP + ATAPI signature              | ✅ `ata_dev_classify()`                | ⬜ §1.1 P0                                      |
| IDENTIFY PACKET DEVICE             | ✅                                     | ✅ `__ata_dev_select()`                | ⬜ §1.2 P0                                      |
| PIO packet protocol                | ✅ (legacy support)                    | ✅ `ide_do_drive_cmd()`                | ⬜ §2.1 P0                                      |
| Bus Master DMA for ATAPI           | ✅                                     | ✅ `ide_dma_setup()`                   | ⬜ §3.1 P2                                      |
| AHCI ATAPI (ACMD field)            | ✅ StorPort miniport                   | ✅ `ahci_exec_polled_cmd()`            | ⬜ §3.2 P2                                      |
| Transport abstraction              | ✅ StorPort / WDF                      | ✅ `ata_std_qc_defer()`                | ⬜ §4.1 P1                                      |
| TEST UNIT READY                    | ✅                                     | ✅ `sr_test_unit_ready()`              | ⬜ §5.1 P1                                      |
| INQUIRY                            | ✅                                     | ✅ `scsi_inquiry()`                    | ⬜ §5.1 P1                                      |
| READ CAPACITY                      | ✅                                     | ✅ `sr_read_capacity()`                | ⬜ §5.2 P0                                      |
| READ (10) / READ (12)              | ✅                                     | ✅ `sr_block_read()`                   | ⬜ §5.2 P0                                      |
| READ DISC STRUCTURE (DVD/BD)       | ✅ (via DeviceIoControl)               | ✅ `sr_do_ioctl()`                     | ⬜ §5.3 P2 (added)                              |
| GET CONFIGURATION / Profiles       | ✅                                     | ✅ `sr_get_config()`                   | ⬜ §5.4 P1                                      |
| MODE SENSE / Capabilities          | ✅                                     | ✅ `sr_mode_sense()`                   | ⬜ §5.5 P2                                      |
| REQUEST SENSE                      | ✅                                     | ✅ `scsi_eh_prep_cmnd()`               | ⬜ §6.1 P1                                      |
| Sense Key / ASC / ASCQ parsing     | ✅                                     | ✅ `scsi_sense_hdr`                    | ⬜ §6.1 P1                                      |
| Media change detection             | ✅ AutoPlay / WM_DEVICECHANGE          | ✅ `sr_check_events()`                 | ⬜ §6.2 P2                                      |
| Eject / Load tray                  | ✅ Explorer context menu               | ✅ `eject` command                     | ⬜ §5.3 P2                                      |
| Tray lock                          | ✅                                     | ✅                                     | ⬜ §5.3 P2                                      |
| READ TOC / multi-session           | ✅                                     | ✅ `sr_read_toc()`                     | ⬜ §5.3 P2                                      |
| Block device registration          | ✅ CdRom class driver                  | ✅ `/dev/sr0`                          | ⬜ §7.1 P1                                      |
| ISO 9660 filesystem                | ✅ CDFS.sys                            | ✅ `isofs` module                      | ⬜ §8.1 P3                                      |
| Joliet Unicode filenames           | ✅                                     | ✅                                     | ⬜ §8.1 P3                                      |
| **Rock Ridge (POSIX on ISO)** ⭐   | ❌ Not supported                       | ✅ `isofs` with RR                     | ⬜ §12.1 P3 — long names + POSIX on CD          |
| **El Torito boot detection**       | ✅ (silent)                            | ✅ `isofs`                             | ⬜ §12.2 P3 — "Bootable" badge in Disk Manager  |
| UDF filesystem                     | ✅ udfs.sys                            | ✅ `udf` module                        | ⬜ §8.2 P3                                      |
| Audio CD playback                  | ✅ Windows Media Player                | ✅ `cdda` + various players            | ⬜ §9.1 P4                                      |
| Audio extraction (ripping)         | ✅ WMP / iTunes                        | ✅ `cdparanoia` / `libcdio`            | ⬜ §9.1 P4                                      |
| **C2 error ptr ripping** ⭐        | ❌ Third-party only (EAC)              | ❌ `cdparanoia` (separate package)     | ⬜ §9.2 P4 — built-in bit-perfect extraction    |
| **Drive speed/noise GUI** ⭐       | ❌ Third-party only (Nero DriveSpeed)  | ❌ `hdparm` CLI only                   | ⬜ §11.1 P4 — native Drive Properties slider    |
| Async I/O                          | ✅ Overlapped I/O                      | ✅ Block MQ                            | ⬜ §10.1 P2                                     |
| **Full optical drive stack**       | ✅                                     | ✅                                     | ⬜ Requires §1–§8 at minimum                    |
