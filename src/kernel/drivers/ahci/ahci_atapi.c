/* ============================================================================
 * ahci_atapi.c -- ATAPI (CD/DVD) device support
 *
 * Under AHCI, the Host Bus Adapter handles the 8-step PIO state machine in
 * hardware.  The driver builds a Command FIS (ATA_CMD_PACKET, 0xA0) and places
 * the SCSI CDB in the ACMD area of the command table.  The HBA then executes:
 *   1. Drive/Head selection (via FIS Device field)
 *   2. BSY/DRQ polling
 *   3. Features/byte-count-limit setup (via FIS fields)
 *   4. 0xA0 command issue
 *   5. DRQ wait + CDB transfer (from ACMD)
 *   6. Data phase (via PRDT, DMA or PIO)
 *   7. Status check
 * This approach is the correct AHCI implementation -- never bitbang legacy
 * IDE task file registers when an AHCI controller is present.
 * ============================================================================ */

#include "kernel/drivers/ahci_internal.h"

/* ---- ATAPI command timeout for optical devices ---- */
#define ATAPI_TIMEOUT_US   30000000  /* 30 seconds for optical spin-up */
#define ATAPI_TIMEOUT_MS   30000     /* 30 seconds for event-based wait */

/* ---- Issue command with ATAPI-specific longer timeout ---- */
static int port_issue_cmd_atapi(struct ahci_port *p, int slot)
{
    volatile uint8_t *pregs = p->regs;
    uint32_t tfd;
    int result = 0;

    /* Issue the command */
    port_write(pregs, AHCI_PxCI, 1U << slot);

    if (use_events) {
        int signalled = event_wait_timeout(&p->completion, ATAPI_TIMEOUT_MS);
        if (!signalled) {
            klog(LOG_WARN, "ahci",
                 "Port %u: ATAPI command timeout (%u ms)",
                 (uint64_t)p->port_num, (uint64_t)ATAPI_TIMEOUT_MS);
            result = -1;
        }
    } else {
        /* Polling mode -- optical drives need up to 30s for spin-up */
        uint32_t timeout = ATAPI_TIMEOUT_US;
        while (timeout--) {
            uint32_t ci = port_read(pregs, AHCI_PxCI);
            if (!(ci & (1U << slot)))
                break;
            tfd = port_read(pregs, AHCI_PxTFD);
            if (tfd & AHCI_PxTFD_ERR) {
                result = -1;
                break;
            }
        }
        if (timeout == 0 && result == 0) {
            klog(LOG_WARN, "ahci",
                 "Port %u: ATAPI command timeout (poll, %u us)",
                 (uint64_t)p->port_num, (uint64_t)ATAPI_TIMEOUT_US);
            result = -1;
        }
        port_write(pregs, AHCI_PxIS, port_read(pregs, AHCI_PxIS));
    }

    /* Check final Task File Data for errors */
    tfd = port_read(pregs, AHCI_PxTFD);
    if (tfd & AHCI_PxTFD_ERR) {
        klog(LOG_DEBUG, "ahci",
             "Port %u: ATAPI TFD error 0x%x",
             (uint64_t)p->port_num, (uint64_t)tfd);
        result = -1;
    }

    /* Attempt CLO reset on failure (no retry -- ATAPI errors need REQUEST SENSE) */
    if (result != 0) {
        p->errors.cmd_failures++;
        tfd = port_read(pregs, AHCI_PxTFD);
        if (tfd & (AHCI_PxTFD_BSY | AHCI_PxTFD_DRQ))
            port_clo_reset(p);
    }

    return result;
}

/* ---- ATAPI: Build PRDT for DMA transfer ----
 *
 * Splits a buffer into PRDT entries respecting AHCI constraints:
 *   - Each PRDT entry: max 64 KB minus 2 bytes (0xFFFE)
 *   - No entry may cross a 64 KB physical alignment boundary
 *   - Last entry gets the Interrupt-on-Completion (IOC) bit (bit 31 of DBC)
 *
 * Returns number of PRDT entries built, or -1 on error.
 */
static int atapi_build_prdt(struct ahci_cmd_tbl *tbl, void *buffer,
                            uint32_t buf_len)
{
    uint32_t remaining = buf_len;
    uintptr_t phys_addr = (uintptr_t)buffer;
    int entry = 0;

    while (remaining > 0 && entry < AHCI_MAX_PRDT) {
        uint32_t chunk = remaining;
        uint32_t boundary_dist;

        /* Distance to next 64 KB boundary */
        boundary_dist = 0x10000 - (uint32_t)(phys_addr & 0xFFFF);
        if (chunk > boundary_dist)
            chunk = boundary_dist;

        /* AHCI PRDT max: 4 MB per entry, but enforce 64 KB - 2 for safety */
        if (chunk > 0xFFFE)
            chunk = 0xFFFE;

        /* DBC is byte count minus 1 (0-based), must be odd for word alignment */
        tbl->prdt[entry].dba  = (uint32_t)(phys_addr & 0xFFFFFFFF);
        tbl->prdt[entry].dbau = 0;  /* 32-bit addresses only */
        tbl->prdt[entry].reserved = 0;
        tbl->prdt[entry].dbc  = chunk - 1;

        remaining -= chunk;
        phys_addr += chunk;
        entry++;
    }

    if (remaining > 0) {
        /* Buffer too large for PRDT -- should never happen with 8 entries x 64KB */
        klog(LOG_WARN, "ahci",
             "ATAPI PRDT overflow: %u bytes remain, %d entries used",
             (uint64_t)remaining, (uint64_t)entry);
        return -1;
    }

    /* Set IOC (Interrupt on Completion) on the last entry */
    if (entry > 0)
        tbl->prdt[entry - 1].dbc |= (1U << 31);

    return entry;
}

/* ---- ATAPI: Send a SCSI packet command ----
 *
 * This function implements the AHCI ATAPI command protocol:
 *   - Builds a Register H2D FIS with ATA_CMD_PACKET (0xA0)
 *   - Sets byte count limit in LBA Mid/High (FIS lba1/lba2)
 *   - Sets Features to 0x01 for DMA mode (AHCI handles DMA natively)
 *   - Copies the CDB into the ACMD area, padded to the device's
 *     expected packet size (12 or 16 bytes, zero-padded)
 *   - Builds multi-entry PRDT respecting 64 KB boundaries
 *   - Sets the ATAPI bit (bit 5) in the command header flags
 *   - Uses a 30-second timeout for optical media spin-up
 *
 * Parameters:
 *   p         -- port with ATAPI device
 *   cdb       -- SCSI Command Descriptor Block
 *   cdb_len   -- CDB length (6, 10, 12, or 16 bytes)
 *   buffer    -- data buffer (NULL for no-data commands)
 *   buf_len   -- buffer size in bytes
 *   direction -- 0 = read (device->host), 1 = write (host->device)
 */
static int atapi_packet_cmd(struct ahci_port *p, const uint8_t *cdb,
                            uint32_t cdb_len, void *buffer, uint32_t buf_len,
                            int direction)
{
    int slot;
    struct ahci_cmd_header *hdr;
    struct ahci_cmd_tbl *tbl;
    struct fis_reg_h2d *fis;
    uint32_t pad_len;
    uint32_t i;

    slot = port_find_slot(p->regs);
    if (slot < 0) return -1;

    hdr = &p->cmdlist[slot];
    tbl = p->cmdtbl[slot];

    ahci_memset(tbl, 0, sizeof(struct ahci_cmd_tbl));

    /* Build Register H2D FIS */
    fis = (struct fis_reg_h2d *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;               /* Command bit set */
    fis->command  = ATA_CMD_PACKET;      /* 0xA0 = PACKET */
    fis->featurel = 1;                   /* Features: DMA=1 (AHCI handles DMA) */
    fis->lba1     = (uint8_t)(buf_len & 0xFF);         /* Byte count limit low */
    fis->lba2     = (uint8_t)((buf_len >> 8) & 0xFF);  /* Byte count limit high */
    fis->device   = 0;

    /* Copy CDB into ACMD area, zero-padded to device's expected packet size.
     * Most ATAPI devices expect 12 bytes; some (tape, etc.) expect 16.
     * The actual CDB may be shorter (e.g., 6-byte SCSI commands). */
    pad_len = (uint32_t)p->atapi_packet_size;
    if (pad_len < 12) pad_len = 12;   /* Minimum 12 bytes per AHCI spec */
    if (pad_len > 16) pad_len = 16;   /* ACMD area is 16 bytes max */
    if (cdb_len > pad_len) cdb_len = pad_len;

    for (i = 0; i < cdb_len; i++)
        tbl->acmd[i] = cdb[i];
    for (; i < pad_len; i++)
        tbl->acmd[i] = 0;             /* Zero-pad remaining bytes */

    /* Build PRDT for DMA data transfer */
    if (buffer && buf_len > 0) {
        int prdt_count = atapi_build_prdt(tbl, buffer, buf_len);
        if (prdt_count < 0) return -1;
        hdr->prdtl = (uint16_t)prdt_count;
    } else {
        hdr->prdtl = 0;
    }

    /* Command header flags:
     *   Bits 4:0 = CFIS length in DWORDs (5 for 20-byte FIS)
     *   Bit 5    = ATAPI (1 = ATAPI command, HBA reads ACMD)
     *   Bit 6    = Write (1 = host->device, 0 = device->host) */
    hdr->flags = ((sizeof(struct fis_reg_h2d) / 4) & 0x1F)
               | (1 << 5);                    /* ATAPI bit */
    if (direction)
        hdr->flags |= (1 << 6);               /* Write direction */
    hdr->prdbc = 0;

    return port_issue_cmd_atapi(p, slot);
}

/* ---- ATAPI: DMA command with contiguous physical buffer ----
 *
 * Higher-level wrapper that allocates a physically-contiguous, DMA-safe
 * buffer via pmm_alloc_contiguous(), issues the ATAPI command via DMA,
 * and copies data back to the caller's buffer.
 *
 * Use this for large transfers where the caller's buffer may not be
 * physically contiguous (e.g., heap-allocated via kmalloc).
 *
 * Returns 0 on success, negative ATAPI_ERR_* on failure.
 */
int atapi_dma_command(struct ahci_port *p, const uint8_t *cdb,
                      uint32_t cdb_len, void *buffer, uint32_t buf_len,
                      int direction)
{
    void *dma_buf;
    uint32_t pages;
    int rc;

    if (!buffer || buf_len == 0) {
        /* No-data command -- just use the regular path */
        return atapi_packet_cmd(p, cdb, cdb_len, (void *)0, 0, direction);
    }

    /* For small transfers (≤ 4KB), use buffer directly -- kmalloc memory
     * is from the kernel's identity-mapped heap, physically contiguous */
    if (buf_len <= 4096)
        return atapi_packet_cmd(p, cdb, cdb_len, buffer, buf_len, direction);

    /* Allocate physically-contiguous DMA buffer via PMM */
    pages = (buf_len + 4095) / 4096;
    dma_buf = (void *)pmm_alloc_contiguous(pages);
    if (!dma_buf) {
        klog(LOG_WARN, "ahci",
             "Port %u: DMA alloc failed (%u pages)",
             (uint64_t)p->port_num, (uint64_t)pages);
        /* Fall back to direct path (may work if buffer happens to be contiguous) */
        return atapi_packet_cmd(p, cdb, cdb_len, buffer, buf_len, direction);
    }

    /* For write commands, copy data into DMA buffer first */
    if (direction) {
        uint8_t *src = (uint8_t *)buffer;
        uint8_t *dst = (uint8_t *)dma_buf;
        uint32_t j;
        for (j = 0; j < buf_len; j++)
            dst[j] = src[j];
    }

    rc = atapi_packet_cmd(p, cdb, cdb_len, dma_buf, buf_len, direction);

    /* For read commands, copy data back from DMA buffer */
    if (rc == 0 && !direction) {
        uint8_t *src = (uint8_t *)dma_buf;
        uint8_t *dst = (uint8_t *)buffer;
        uint32_t j;
        for (j = 0; j < buf_len; j++)
            dst[j] = src[j];
    }

    /* Free DMA buffer page-by-page */
    {
        uintptr_t addr = (uintptr_t)dma_buf;
        uint32_t j;
        for (j = 0; j < pages; j++) {
            pmm_free_frame(addr);
            addr += 4096;
        }
    }

    return rc;
}

/* ============================================================================
 * REQUEST SENSE -- SCSI error information retrieval
 *
 * When any ATAPI command fails (ERR bit in Task File Data), the device caches
 * detailed error information internally.  The driver must issue REQUEST SENSE
 * (0x03) to retrieve 18 bytes of fixed-format sense data containing:
 *   Byte 2  bits 3-0 : Sense Key (error category)
 *   Byte 12          : Additional Sense Code (ASC)
 *   Byte 13          : Additional Sense Code Qualifier (ASCQ)
 * ============================================================================ */

/* ---- Sense key name for logging ---- */
static const char *atapi_sense_name(uint8_t sense_key)
{
    switch (sense_key) {
    case SCSI_SK_NO_SENSE:        return "No Sense";
    case SCSI_SK_RECOVERED:       return "Recovered";
    case SCSI_SK_NOT_READY:       return "Not Ready";
    case SCSI_SK_MEDIUM_ERROR:    return "Medium Error";
    case SCSI_SK_HARDWARE_ERROR:  return "Hardware Error";
    case SCSI_SK_ILLEGAL_REQUEST: return "Illegal Request";
    case SCSI_SK_UNIT_ATTENTION:  return "Unit Attention";
    case SCSI_SK_DATA_PROTECT:    return "Data Protect";
    case SCSI_SK_BLANK_CHECK:     return "Blank Check";
    case SCSI_SK_ABORTED_COMMAND: return "Aborted Command";
    default:                      return "Unknown";
    }
}

/* ---- Classify Sense Key / ASC / ASCQ into an ATAPI error code ---- */
static int atapi_classify_sense(uint8_t sense_key, uint8_t asc, uint8_t ascq)
{
    switch (sense_key) {
    case SCSI_SK_NO_SENSE:
        return ATAPI_OK;

    case SCSI_SK_RECOVERED:
        /* Recovered error -- data is valid, log warning */
        return ATAPI_OK;

    case SCSI_SK_NOT_READY:
        if (asc == SCSI_ASC_NO_MEDIUM)
            return ATAPI_ERR_NOMEDIUM;           /* 0x3A/xx -- no disc */
        if (asc == SCSI_ASC_BECOMING_READY) {
            if (ascq == 0x01)
                return ATAPI_ERR_BECOMING;        /* 0x04/01 -- spinning up */
            if (ascq == 0x02)
                return ATAPI_ERR_BECOMING;        /* 0x04/02 -- start unit needed */
            return ATAPI_ERR_BECOMING;            /* 0x04/xx -- not ready */
        }
        return ATAPI_ERR_NOMEDIUM;               /* Other not-ready */

    case SCSI_SK_MEDIUM_ERROR:
        return ATAPI_ERR_IO;                     /* Scratched/unreadable media */

    case SCSI_SK_HARDWARE_ERROR:
        return ATAPI_ERR_IO;                     /* Internal drive failure */

    case SCSI_SK_ILLEGAL_REQUEST:
        if (asc == SCSI_ASC_INVALID_OPCODE)
            return ATAPI_ERR_INVALID;            /* 0x20/00 -- bad command */
        if (asc == SCSI_ASC_INVALID_FIELD)
            return ATAPI_ERR_INVALID;            /* 0x24/00 -- bad field */
        if (asc == 0x26)
            return ATAPI_ERR_INVALID;            /* 0x26/00 -- bad parameter */
        return ATAPI_ERR_INVALID;

    case SCSI_SK_UNIT_ATTENTION:
        if (asc == SCSI_ASC_MEDIA_CHANGED)
            return ATAPI_ERR_MEDIACHANGE;        /* 0x28/00 -- disc changed */
        if (asc == SCSI_ASC_POWER_ON)
            return ATAPI_ERR_MEDIACHANGE;        /* 0x29/00 -- power-on/reset */
        return ATAPI_ERR_MEDIACHANGE;            /* Other unit attention */

    case SCSI_SK_DATA_PROTECT:
        return ATAPI_ERR_INVALID;                /* Write-protected */

    case SCSI_SK_BLANK_CHECK:
        return ATAPI_ERR_IO;                     /* Blank/empty medium area */

    case SCSI_SK_ABORTED_COMMAND:
        return ATAPI_ERR_ABORTED;                /* Retry candidate */

    default:
        return ATAPI_ERR_IO;
    }
}

/* ---- Issue REQUEST SENSE and return classified error code ---- */
int atapi_request_sense(struct ahci_port *p)
{
    uint8_t cdb[12];
    uint8_t *sense;
    uint8_t response_code, sense_key, asc, ascq, add_len;
    int result;

    sense = (uint8_t *)kmalloc(18);
    if (!sense) return ATAPI_ERR_SENSE_FAIL;
    ahci_memset(sense, 0, 18);
    ahci_memset(cdb, 0, 12);

    /* CDB for REQUEST SENSE: opcode=0x03, allocation_length=18 */
    cdb[0] = SCSI_REQUEST_SENSE;
    cdb[4] = 18;                          /* Allocation length */

    if (atapi_packet_cmd(p, cdb, 6, sense, 18, 0) != 0) {
        kfree(sense);
        return ATAPI_ERR_SENSE_FAIL;
    }

    /* Parse fixed-format sense data (SPC-4 S4.5.3) */
    response_code = sense[0] & 0x7F;
    sense_key     = sense[2] & 0x0F;
    add_len       = sense[7];             /* Additional sense length */
    asc           = (add_len >= 5) ? sense[12] : 0;
    ascq          = (add_len >= 6) ? sense[13] : 0;

    /* Log sense data */
    klog(LOG_DEBUG, "ahci",
         "Port %u: Sense key=0x%x ASC=0x%x ASCQ=0x%x (%s)",
         (uint64_t)p->port_num, (uint64_t)sense_key,
         (uint64_t)asc, (uint64_t)ascq, atapi_sense_name(sense_key));

    /* Warn on deferred errors (response code 0x71) */
    if (response_code == 0x71) {
        klog(LOG_DEBUG, "ahci",
             "Port %u: deferred sense (response=0x71)", (uint64_t)p->port_num);
    }

    kfree(sense);

    result = atapi_classify_sense(sense_key, asc, ascq);
    return result;
}

/* ---- Simple busy-wait delay (microseconds) ----
 * Used during early init when the scheduler may not be available yet.
 * Reads the port's Alternate Status register to consume ~100ns per read. */
static void atapi_delay_us(volatile uint8_t *pregs, uint32_t us)
{
    /* Each port_read takes ~100ns on modern hardware; 10 reads ≈ 1us */
    uint32_t loops = us * 10;
    while (loops--)
        port_read(pregs, AHCI_PxTFD);
}

/* ---- ATAPI: TEST UNIT READY ----
 *
 * CDB 0x00 -- no data transfer. Checks if the device has media loaded and
 * is ready to accept commands. Must be retried during optical spin-up.
 *
 * Returns:
 *   ATAPI_OK          -- device ready
 *   ATAPI_ERR_NOMEDIUM -- no disc in drive
 *   ATAPI_ERR_BECOMING -- still spinning up (caller should retry)
 *   Other ATAPI_ERR_*  -- sense-classified error
 */
int atapi_test_unit_ready(struct ahci_port *p)
{
    uint8_t cdb[12];
    int retries = 10;
    int rc, sense_rc;

    ahci_memset(cdb, 0, 12);
    cdb[0] = SCSI_TEST_UNIT_READY;

    while (retries > 0) {
        rc = atapi_packet_cmd(p, cdb, 6, (void *)0, 0, 0);
        if (rc == 0)
            return ATAPI_OK;    /* Device is ready */

        /* Command failed -- get sense data for error classification */
        sense_rc = atapi_request_sense(p);

        if (sense_rc == ATAPI_ERR_BECOMING) {
            /* Drive is spinning up -- wait 500ms and retry */
            retries--;
            if (retries > 0) {
                klog(LOG_DEBUG, "ahci",
                     "Port %u: TUR -- becoming ready, %d retries left",
                     (uint64_t)p->port_num, (uint64_t)retries);
                atapi_delay_us(p->regs, 500000);  /* 500ms */
            }
            continue;
        }

        /* Non-transient error -- return immediately */
        return sense_rc;
    }

    klog(LOG_WARN, "ahci",
         "Port %u: TEST UNIT READY exhausted retries", (uint64_t)p->port_num);
    return ATAPI_ERR_BECOMING;
}

/* ---- ATAPI: INQUIRY ----
 *
 * CDB 0x12 -- retrieves 36 bytes of device identification:
 *   Byte 0  bits 4-0: peripheral device type (0x05 = CD/DVD)
 *   Byte 0  bits 7-5: peripheral qualifier (0 = connected)
 *   Bytes 8-15:  vendor identification (8 bytes, ASCII, space-padded)
 *   Bytes 16-31: product identification (16 bytes, ASCII, space-padded)
 *   Bytes 32-35: product revision level (4 bytes, ASCII, space-padded)
 */
int atapi_inquiry(struct ahci_port *p)
{
    uint8_t cdb[12];
    uint8_t *resp;
    int i;

    resp = (uint8_t *)kmalloc(36);
    if (!resp) return -1;
    ahci_memset(resp, 0, 36);
    ahci_memset(cdb, 0, 12);

    cdb[0] = SCSI_INQUIRY;
    cdb[4] = 36;                /* Allocation length */

    if (atapi_packet_cmd(p, cdb, 6, resp, 36, 0) != 0) {
        int sense_rc = atapi_request_sense(p);
        kfree(resp);
        return sense_rc < 0 ? sense_rc : -1;
    }

    /* Parse peripheral device type (byte 0, bits 4-0) */
    {
        uint8_t pdt = resp[0] & 0x1F;
        uint8_t pq  = (resp[0] >> 5) & 0x07;
        if (pq != 0) {
            klog(LOG_DEBUG, "ahci",
                 "Port %u: INQUIRY qualifier=%u (device may not be connected)",
                 (uint64_t)p->port_num, (uint64_t)pq);
        }
        /* Update SCSI type from INQUIRY (more authoritative than IDENTIFY) */
        p->atapi_scsi_type = pdt;
    }

    /* Vendor identification (bytes 8-15, 8 chars, space-padded) */
    for (i = 0; i < 8; i++)
        p->vendor[i] = (char)resp[8 + i];
    p->vendor[8] = '\0';
    for (i = 7; i > 0; i--) {
        if (p->vendor[i] == ' ') p->vendor[i] = '\0';
        else break;
    }

    /* Product identification (bytes 16-31, 16 chars, space-padded) */
    for (i = 0; i < 16; i++)
        p->product[i] = (char)resp[16 + i];
    p->product[16] = '\0';
    for (i = 15; i > 0; i--) {
        if (p->product[i] == ' ') p->product[i] = '\0';
        else break;
    }

    /* Product revision (bytes 32-35, 4 chars, space-padded) */
    for (i = 0; i < 4; i++)
        p->revision[i] = (char)resp[32 + i];
    p->revision[4] = '\0';
    for (i = 3; i > 0; i--) {
        if (p->revision[i] == ' ') p->revision[i] = '\0';
        else break;
    }

    kfree(resp);

    klog(LOG_INFO, "ahci",
         "ATAPI port %u: INQUIRY: %s %s rev %s",
         (uint64_t)p->port_num, p->vendor, p->product, p->revision);

    return 0;
}

/* ---- Human-readable profile name ---- */
static const char *atapi_profile_name(uint16_t profile)
{
    switch (profile) {
    case MMC_PROF_NONE:           return "No media";
    case MMC_PROF_CD_ROM:         return "CD-ROM";
    case MMC_PROF_CD_R:           return "CD-R";
    case MMC_PROF_CD_RW:          return "CD-RW";
    case MMC_PROF_DVD_ROM:        return "DVD-ROM";
    case MMC_PROF_DVD_R:          return "DVD-R";
    case MMC_PROF_DVD_RAM:        return "DVD-RAM";
    case MMC_PROF_DVD_RW_RO:      return "DVD-RW (RO)";
    case MMC_PROF_DVD_RW_SEQ:     return "DVD-RW (Seq)";
    case MMC_PROF_DVD_R_DL_SEQ:   return "DVD-R DL";
    case MMC_PROF_DVD_PLUS_RW:    return "DVD+RW";
    case MMC_PROF_DVD_PLUS_R:     return "DVD+R";
    case MMC_PROF_DVD_PLUS_RW_DL: return "DVD+RW DL";
    case MMC_PROF_DVD_PLUS_R_DL:  return "DVD+R DL";
    case MMC_PROF_BD_ROM:         return "BD-ROM";
    case MMC_PROF_BD_R_SRM:       return "BD-R (SRM)";
    case MMC_PROF_BD_R_RRM:       return "BD-R (RRM)";
    case MMC_PROF_BD_RE:          return "BD-RE";
    default:                      return "Unknown";
    }
}

/* ---- Map a profile code to capability flags ---- */
static uint32_t atapi_profile_to_cap(uint16_t profile)
{
    switch (profile) {
    case MMC_PROF_CD_ROM:
        return ATAPI_CAP_CD_READ;
    case MMC_PROF_CD_R:
    case MMC_PROF_CD_RW:
        return ATAPI_CAP_CD_READ | ATAPI_CAP_CD_WRITE;
    case MMC_PROF_DVD_ROM:
        return ATAPI_CAP_DVD_READ;
    case MMC_PROF_DVD_R:
    case MMC_PROF_DVD_RAM:
    case MMC_PROF_DVD_RW_RO:
    case MMC_PROF_DVD_RW_SEQ:
    case MMC_PROF_DVD_R_DL_SEQ:
    case MMC_PROF_DVD_R_DL_LJ:
    case MMC_PROF_DVD_PLUS_RW:
    case MMC_PROF_DVD_PLUS_R:
    case MMC_PROF_DVD_PLUS_RW_DL:
    case MMC_PROF_DVD_PLUS_R_DL:
        return ATAPI_CAP_DVD_READ | ATAPI_CAP_DVD_WRITE;
    case MMC_PROF_BD_ROM:
        return ATAPI_CAP_BD_READ;
    case MMC_PROF_BD_R_SRM:
    case MMC_PROF_BD_R_RRM:
    case MMC_PROF_BD_RE:
        return ATAPI_CAP_BD_READ | ATAPI_CAP_BD_WRITE;
    default:
        return 0;
    }
}

/* ---- ATAPI: GET CONFIGURATION ----
 *
 * CDB 0x46 -- retrieves the drive's MMC profile list and active features.
 * The profile list indicates what disc types the drive supports (CD, DVD, BD)
 * and the current profile indicates what type of media is currently inserted.
 *
 * CDB format: { 0x46, RT, Start[1], Start[0], 0, 0, 0, Len[1], Len[0], 0, 0, 0 }
 *   RT=0x00: return all features from start_feature onward
 *   RT=0x01: return only current (active) features
 *   RT=0x02: return one specific feature only
 *
 * Response starts with 8-byte Feature Header:
 *   Bytes 0-3 (BE): Data length (total response minus these 4 bytes)
 *   Bytes 6-7 (BE): Current profile code
 * Followed by Feature Descriptors, each starting with:
 *   Bytes 0-1 (BE): Feature code
 *   Byte 2: version/persistent/current flags
 *   Byte 3: Additional length
 *   Bytes 4+: Feature-specific data
 *
 * Feature 0x0000 (Profile List) contains 4-byte profile descriptors:
 *   Bytes 0-1 (BE): Profile number
 *   Byte 2 bit 0: 1 = this profile is currently active
 */
int atapi_get_configuration(struct ahci_port *p)
{
    uint8_t cdb[12];
    uint8_t *resp;
    uint32_t data_len, offset;
    uint16_t current_profile;
    const char *drive_desc;

    /* Allocate response buffer -- 512 bytes is sufficient for profile list */
    resp = (uint8_t *)kmalloc(512);
    if (!resp) return -1;
    ahci_memset(resp, 0, 512);
    ahci_memset(cdb, 0, 12);

    /* GET CONFIGURATION: RT=0x00 (all features), starting from feature 0 */
    cdb[0] = SCSI_GET_CONFIGURATION;
    cdb[1] = 0x00;                   /* RT = 0 (all features) */
    cdb[2] = 0x00;                   /* Starting Feature Number (high) */
    cdb[3] = 0x00;                   /* Starting Feature Number (low) */
    cdb[7] = (uint8_t)((512 >> 8) & 0xFF);  /* Allocation length high */
    cdb[8] = (uint8_t)(512 & 0xFF);          /* Allocation length low */

    if (atapi_packet_cmd(p, cdb, 10, resp, 512, 0) != 0) {
        int sense_rc = atapi_request_sense(p);
        kfree(resp);
        return sense_rc < 0 ? sense_rc : -1;
    }

    /* Parse Feature Header (8 bytes) */
    data_len = ((uint32_t)resp[0] << 24) | ((uint32_t)resp[1] << 16)
             | ((uint32_t)resp[2] << 8)  | (uint32_t)resp[3];
    current_profile = ((uint16_t)resp[6] << 8) | (uint16_t)resp[7];

    p->current_profile = current_profile;
    p->profile_flags = 0;
    p->drive_type = ATAPI_DRIVE_UNKNOWN;

    /* Add current profile's capabilities */
    p->profile_flags |= atapi_profile_to_cap(current_profile);

    /* Clamp data_len to buffer size */
    if (data_len > 508)
        data_len = 508;   /* 512 - 4 byte header */

    /* Walk Feature Descriptors starting at offset 8 */
    offset = 8;
    while (offset + 4 <= data_len + 4) {
        uint16_t feat_code;
        uint8_t  add_len;

        feat_code = ((uint16_t)resp[offset] << 8) | (uint16_t)resp[offset + 1];
        add_len   = resp[offset + 3];

        if (feat_code == 0x0000) {
            /* Profile List feature -- parse profile descriptors */
            uint32_t prof_offset = offset + 4;
            uint32_t prof_end    = prof_offset + add_len;
            if (prof_end > data_len + 4)
                prof_end = data_len + 4;

            while (prof_offset + 4 <= prof_end) {
                uint16_t prof_num;
                prof_num = ((uint16_t)resp[prof_offset] << 8)
                         | (uint16_t)resp[prof_offset + 1];
                /* Accumulate capabilities for every supported profile */
                p->profile_flags |= atapi_profile_to_cap(prof_num);
                prof_offset += 4;
            }
        }

        /* Advance to next feature descriptor */
        offset += 4 + add_len;
        if (add_len == 0 && offset > 8)
            break;  /* Safety: avoid infinite loop on malformed data */
    }

    kfree(resp);

    /* Derive drive type from capability flags */
    if (p->profile_flags & (ATAPI_CAP_BD_READ | ATAPI_CAP_BD_WRITE))
        p->drive_type = ATAPI_DRIVE_BD_COMBO;
    else if (p->profile_flags & (ATAPI_CAP_DVD_READ | ATAPI_CAP_DVD_WRITE))
        p->drive_type = ATAPI_DRIVE_DVD_COMBO;
    else if (p->profile_flags & (ATAPI_CAP_CD_READ | ATAPI_CAP_CD_WRITE))
        p->drive_type = ATAPI_DRIVE_CD_ONLY;

    /* Build drive capabilities description */
    switch (p->drive_type) {
    case ATAPI_DRIVE_BD_COMBO:  drive_desc = "BD combo"; break;
    case ATAPI_DRIVE_DVD_COMBO: drive_desc = "DVD combo"; break;
    case ATAPI_DRIVE_CD_ONLY:   drive_desc = "CD-only"; break;
    default:                    drive_desc = "unknown"; break;
    }

    klog(LOG_INFO, "ahci",
         "ATAPI port %u: %s drive, current media: %s",
         (uint64_t)p->port_num, drive_desc,
         atapi_profile_name(current_profile));

    return 0;
}

/* ---- ATAPI: IDENTIFY PACKET DEVICE ---- */
int atapi_do_identify(struct ahci_port *p)
{
    int slot;
    struct ahci_cmd_header *hdr;
    struct ahci_cmd_tbl *tbl;
    struct fis_reg_h2d *fis;
    uint16_t *ident_buf;
    int i;

    ident_buf = (uint16_t *)kmalloc(512);
    if (!ident_buf) return -1;
    ahci_memset(ident_buf, 0, 512);

    slot = port_find_slot(p->regs);
    if (slot < 0) { return -1; }

    hdr = &p->cmdlist[slot];
    tbl = p->cmdtbl[slot];
    ahci_memset(tbl, 0, sizeof(struct ahci_cmd_tbl));

    fis = (struct fis_reg_h2d *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;
    fis->command  = ATA_CMD_IDENTIFY_PACKET;
    fis->device   = 0;

    tbl->prdt[0].dba  = (uint32_t)(uintptr_t)ident_buf;
    tbl->prdt[0].dbau = 0;
    tbl->prdt[0].dbc  = 512 - 1;

    hdr->flags = (sizeof(struct fis_reg_h2d) / 4) & 0x1F;
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    if (port_issue_cmd(p, slot) != 0) {
        return -1;
    }

    /* ---- Word 0: General Configuration ---- */
    {
        uint16_t w0 = ident_buf[0];

        /* Bits 15-14: protocol type (10b = ATAPI) */
        if (((w0 >> 14) & 0x3) != 0x2) {
            klog(LOG_WARN, "ahci",
                 "Port %u: IDENTIFY PACKET w0=0x%x -- not ATAPI protocol",
                 (uint64_t)p->port_num, (uint64_t)w0);
        }

        /* Bits 12-8: SCSI peripheral device type */
        p->atapi_scsi_type = (uint8_t)((w0 >> 8) & 0x1F);

        /* Bits 6-5: DRQ timing (00=slow 3ms, 01=IRQ within 50us, 10=accelerated 50us) */
        p->atapi_drq_type = (uint8_t)((w0 >> 5) & 0x3);

        /* Bits 1-0: command packet size (00=12 bytes, 01=16 bytes) */
        p->atapi_packet_size = (w0 & 0x3) == 0x01 ? 16 : 12;
    }

    /* ---- Words 10-19: Serial number (20 chars, byte-swapped) ---- */
    for (i = 0; i < 10; i++) {
        p->serial[i * 2]     = (char)(ident_buf[10 + i] >> 8);
        p->serial[i * 2 + 1] = (char)(ident_buf[10 + i] & 0xFF);
    }
    p->serial[20] = '\0';
    for (i = 19; i > 0; i--) {
        if (p->serial[i] == ' ') p->serial[i] = '\0';
        else break;
    }

    /* ---- Words 23-26: Firmware revision (8 chars, byte-swapped) ---- */
    for (i = 0; i < 4; i++) {
        p->firmware[i * 2]     = (char)(ident_buf[23 + i] >> 8);
        p->firmware[i * 2 + 1] = (char)(ident_buf[23 + i] & 0xFF);
    }
    p->firmware[8] = '\0';
    for (i = 7; i > 0; i--) {
        if (p->firmware[i] == ' ') p->firmware[i] = '\0';
        else break;
    }

    /* ---- Words 27-46: Model number (40 chars, byte-swapped) ---- */
    for (i = 0; i < 20; i++) {
        p->model[i * 2]     = (char)(ident_buf[27 + i] >> 8);
        p->model[i * 2 + 1] = (char)(ident_buf[27 + i] & 0xFF);
    }
    p->model[40] = '\0';
    for (i = 39; i > 0; i--) {
        if (p->model[i] == ' ') p->model[i] = '\0';
        else break;
    }

    /* ---- Word 49: Capabilities ---- */
    /* bit 8 = LBA supported, bit 11 = IORDY supported (informational) */

    /* ---- Word 63: Multiword DMA modes ---- */
    {
        uint16_t w63 = ident_buf[63];
        uint8_t supported = (uint8_t)(w63 & 0x07);

        p->atapi_dma_mode = 0;
        if (supported & 0x04)      p->atapi_dma_mode = 2;
        else if (supported & 0x02) p->atapi_dma_mode = 1;
        else if (supported & 0x01) p->atapi_dma_mode = 0;
    }

    /* ---- Word 88: Ultra DMA modes ---- */
    {
        uint16_t w88 = ident_buf[88];
        uint8_t supported = (uint8_t)(w88 & 0x7F);

        p->atapi_udma_mode = 0xFF;  /* None */
        if (supported) {
            /* Find highest set bit (mode 6 -> UDMA/133, down to 0 -> UDMA/16) */
            int m;
            for (m = 6; m >= 0; m--) {
                if (supported & (1U << m)) {
                    p->atapi_udma_mode = (uint8_t)m;
                    break;
                }
            }
        }
    }

    /* ---- Word 76: SATA capabilities ---- */
    {
        uint16_t w76 = ident_buf[76];
        /* 0x0000 and 0xFFFF mean "not reported" per ACS spec */
        p->atapi_sata_caps = (w76 == 0x0000 || w76 == 0xFFFF) ? 0 : w76;
    }

    p->sectors = 0;  /* Set later by READ CAPACITY */

    /* ---- Log device summary ---- */
    {
        const char *type_name;
        const char *dma_name;

        switch (p->atapi_scsi_type) {
        case 0x05: type_name = "CD/DVD-ROM";     break;
        case 0x00: type_name = "direct-access";  break;
        case 0x01: type_name = "tape";           break;
        case 0x07: type_name = "optical-memory"; break;
        default:   type_name = "unknown";        break;
        }

        if (p->atapi_udma_mode != 0xFF)
            dma_name = "UDMA";
        else if (p->atapi_dma_mode > 0)
            dma_name = "MDMA";
        else
            dma_name = "PIO";

        klog(LOG_INFO, "ahci",
             "ATAPI port %u: \"%s\" type=%s, pkt=%u, DMA=%s",
             (uint64_t)p->port_num, p->model,
             type_name, (uint64_t)p->atapi_packet_size, dma_name);
    }

    return 0;
}

/* ---- ATAPI: READ CAPACITY (10) ----
 *
 * Returns 8 bytes Big-Endian: Last LBA (4 bytes) + Block Size (4 bytes).
 * Both fields MUST be byte-swapped on x86 (Little-Endian).
 * Typical block sizes: 2048 (data CD/DVD), 2352 (raw audio), 512 (rare).
 */
int atapi_read_capacity(struct ahci_port *p)
{
    uint8_t cdb[12];
    uint8_t *resp;
    uint32_t last_lba, block_size;
    uint64_t capacity_mb;

    resp = (uint8_t *)kmalloc(8);
    if (!resp) return -1;
    ahci_memset(resp, 0, 8);
    ahci_memset(cdb, 0, 12);

    cdb[0] = SCSI_READ_CAPACITY;

    if (atapi_packet_cmd(p, cdb, 10, resp, 8, 0) != 0) {
        /* Command failed -- issue REQUEST SENSE for details */
        int sense_err = atapi_request_sense(p);
        kfree(resp);
        if (sense_err == ATAPI_ERR_NOMEDIUM)
            klog(LOG_DEBUG, "ahci",
                 "Port %u: READ CAPACITY -- no medium", (uint64_t)p->port_num);
        return sense_err < 0 ? sense_err : -1;
    }

    /* Big-Endian to Little-Endian byte-swap */
    last_lba   = ((uint32_t)resp[0] << 24) | ((uint32_t)resp[1] << 16)
               | ((uint32_t)resp[2] << 8)  | (uint32_t)resp[3];
    block_size = ((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16)
               | ((uint32_t)resp[6] << 8)  | (uint32_t)resp[7];

    kfree(resp);

    /* Validate block size */
    if (block_size == 0) {
        klog(LOG_WARN, "ahci",
             "Port %u: READ CAPACITY returned block_size=0, defaulting to 2048",
             (uint64_t)p->port_num);
        block_size = 2048;
    } else if (block_size != 2048 && block_size != 2352 && block_size != 512) {
        klog(LOG_DEBUG, "ahci",
             "Port %u: unusual block_size=%u (expected 2048/2352/512)",
             (uint64_t)p->port_num, (uint64_t)block_size);
    }

    p->sectors     = (uint64_t)last_lba + 1;
    p->sector_size = block_size;

    /* Log capacity */
    capacity_mb = (p->sectors * (uint64_t)block_size) / (1024 * 1024);
    klog(LOG_INFO, "ahci",
         "ATAPI port %u: %u MB (%u blocks x %u bytes)",
         (uint64_t)p->port_num, capacity_mb,
         (uint64_t)p->sectors, (uint64_t)block_size);

    return 0;
}

/* ---- ATAPI: READ (10) ----
 *
 * CDB format: { 0x28, flags, LBA[3], LBA[2], LBA[1], LBA[0],
 *               group, Len[1], Len[0], control, 0, 0 }
 * LBA and transfer length are Big-Endian in the CDB.
 * Transfer length is in BLOCKS (not bytes).
 * Maximum 65535 blocks per READ(10) (16-bit field).
 */
int atapi_do_read(struct ahci_port *p, uint64_t lba, uint32_t count,
                  void *buffer)
{
    uint8_t cdb[12];
    uint32_t byte_count;
    uint32_t lba32;
    int rc;

    /* For >65535 blocks, use READ(12) which has a 32-bit transfer length */
    if (count > 65535)
        return atapi_do_read12(p, lba, count, buffer);

    byte_count = count * p->sector_size;
    lba32 = (uint32_t)lba;

    ahci_memset(cdb, 0, 12);
    cdb[0] = SCSI_READ_10;
    cdb[2] = (uint8_t)((lba32 >> 24) & 0xFF);
    cdb[3] = (uint8_t)((lba32 >> 16) & 0xFF);
    cdb[4] = (uint8_t)((lba32 >> 8) & 0xFF);
    cdb[5] = (uint8_t)(lba32 & 0xFF);
    cdb[7] = (uint8_t)((count >> 8) & 0xFF);
    cdb[8] = (uint8_t)(count & 0xFF);

    rc = atapi_packet_cmd(p, cdb, 10, buffer, byte_count, 0);
    if (rc != 0) {
        /* Command failed -- issue REQUEST SENSE for error classification */
        return atapi_request_sense(p);
    }

    return 0;
}

/* ---- ATAPI: READ (12) ----
 *
 * CDB format: { 0xA8, flags, LBA[3], LBA[2], LBA[1], LBA[0],
 *               Len[3], Len[2], Len[1], Len[0], 0, control }
 * For transfer lengths exceeding READ(10)'s 16-bit (65535 block) limit.
 * Splits into multiple READ(12) commands at 65535-block chunks to
 * avoid oversized single DMA transfers.
 */
int atapi_do_read12(struct ahci_port *p, uint64_t lba, uint32_t count,
                    void *buffer)
{
    uint32_t remaining = count;
    uint64_t cur_lba = lba;
    uint8_t *dst = (uint8_t *)buffer;

    while (remaining > 0) {
        uint8_t cdb[12];
        uint32_t chunk = remaining;
        uint32_t byte_count;
        uint32_t lba32;
        int rc;

        /* Cap each transfer to 65535 blocks to keep DMA manageable */
        if (chunk > 65535)
            chunk = 65535;

        byte_count = chunk * p->sector_size;
        lba32 = (uint32_t)cur_lba;

        ahci_memset(cdb, 0, 12);
        cdb[0]  = SCSI_READ_12;
        cdb[2]  = (uint8_t)((lba32 >> 24) & 0xFF);
        cdb[3]  = (uint8_t)((lba32 >> 16) & 0xFF);
        cdb[4]  = (uint8_t)((lba32 >> 8) & 0xFF);
        cdb[5]  = (uint8_t)(lba32 & 0xFF);
        cdb[6]  = (uint8_t)((chunk >> 24) & 0xFF);
        cdb[7]  = (uint8_t)((chunk >> 16) & 0xFF);
        cdb[8]  = (uint8_t)((chunk >> 8) & 0xFF);
        cdb[9]  = (uint8_t)(chunk & 0xFF);

        rc = atapi_packet_cmd(p, cdb, 12, dst, byte_count, 0);
        if (rc != 0)
            return atapi_request_sense(p);

        remaining -= chunk;
        cur_lba   += chunk;
        dst       += byte_count;
    }

    return 0;
}

/* ---- ATAPI public API ---- */

int ahci_atapi_count(void)
{
    return num_atapi;
}

int ahci_atapi_read(int atapi_idx, uint64_t lba, uint32_t count, void *buffer)
{
    int pi;
    if (atapi_idx < 0 || atapi_idx >= num_atapi)
        return -1;
    pi = atapi_map[atapi_idx];
    if (!ports[pi].active || !ports[pi].is_atapi)
        return -1;
    return atapi_do_read(&ports[pi], lba, count, buffer);
}

uint64_t ahci_atapi_capacity(int atapi_idx)
{
    int pi;
    if (atapi_idx < 0 || atapi_idx >= num_atapi)
        return 0;
    pi = atapi_map[atapi_idx];
    return ports[pi].sectors;
}

uint32_t ahci_atapi_sector_size(int atapi_idx)
{
    int pi;
    if (atapi_idx < 0 || atapi_idx >= num_atapi)
        return 0;
    pi = atapi_map[atapi_idx];
    return ports[pi].sector_size;
}
