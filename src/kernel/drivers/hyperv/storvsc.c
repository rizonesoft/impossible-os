/* ============================================================================
 * storvsc.c -- Hyper-V Synthetic SCSI Storage Driver (StorVSC)
 *
 * Clean-room implementation from the public Hyper-V Top-Level Functional
 * Specification (TLFS). NO code derived from Linux hv_storvsc.c (GPL).
 *
 * Protocol flow:
 *   1. Find storage VMBus channel by GUID
 *   2. Open the channel (ring buffers)
 *   3. BEGIN_INITIALIZATION → QUERY_PROTOCOL_VERSION → QUERY_PROPERTIES
 *      → END_INITIALIZATION
 *   4. SCSI INQUIRY → identify disk
 *   5. SCSI READ_CAPACITY(16) → get sector count + size
 *   6. Register as blkdev "hyperv0"
 *
 * Reference: https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/tlfs
 * ============================================================================ */

#include "kernel/drivers/hyperv/storvsc.h"
#include "kernel/drivers/hyperv/vmbus.h"
#include "kernel/drivers/blkdev.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"
#include "kernel/printk.h"

/* ---- Local helpers ---- */

static void storvsc_memset(void *dst, uint8_t val, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--)
        *d++ = val;
}

static void storvsc_memcpy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--)
        *d++ = *s++;
}

/* ---- State ---- */

static struct vmbus_channel *stor_channel;
static struct storvsc_disk_info disk_info[STORVSC_MAX_DEVICES];
static int disk_count;             /* number of discovered devices */
static int storvsc_initialized;

/* Properties from QUERY_PROPERTIES */
static uint8_t prop_max_targets;   /* max targets per path */
static uint8_t prop_max_luns;      /* max LUNs per target */

/* Data buffer for SCSI read/write -- allocated from PMM (identity-mapped)
 * and shared with the host via a separate GPADL. The VMBus ring buffer
 * carries only the VSTOR_PACKET metadata; actual disk data goes through
 * this GPADL-shared transfer buffer. */
#define STORVSC_XFER_PAGES      16  /* 64 KiB = 16 pages */
static void *xfer_buffer;
static uint32_t xfer_gpadl_handle;  /* GPADL handle for xfer_buffer */

/* Transaction ID counter for storvsc requests */
static uint64_t storvsc_next_tid = 0x1000;

/* Ring buffer page count: 32 pages = 16 send + 16 recv = 64 KiB each */
#define STORVSC_RING_PAGES      32

/* Forward declarations (defined after storvsc_enumerate) */
static int storvsc_blk_read(uint64_t lba, uint32_t count, void *buf, void *driver_data);
static int storvsc_blk_write(uint64_t lba, uint32_t count, const void *buf, void *driver_data);

/* ---- Protocol helpers ---- */

/* Send a VSTOR_PACKET (in-band, no data buffer) and wait for response.
 * Used for protocol init messages. Response is written back into pkt. */
static int storvsc_send_packet(struct vstor_packet *pkt)
{
    struct vstor_packet response;
    uint32_t bytes_read;
    uint32_t attempts;
    uint64_t tid = storvsc_next_tid++;

    pkt->flags |= VSTOR_FLAG_REQUEST_COMPLETION;

    /* Send as in-band data packet with proper vmpacket_descriptor framing */
    if (vmbus_sendpacket(stor_channel, pkt,
                         (uint32_t)sizeof(struct vstor_packet),
                         tid,
                         VMBUS_PACKET_TYPE_DATA_INBAND,
                         VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED) < 0) {
        klog(LOG_ERROR, "storvsc", "sendpacket failed for op %u",
             (uint64_t)pkt->operation);
        return -1;
    }

    /* Signal the host */
    vmbus_signal_channel(stor_channel);

    /* Poll for response (NO PAUSE -- avoids Hyper-V PLE slowdown) */
    for (attempts = 0; attempts < 5000000; attempts++) {
        __asm__ volatile("" ::: "memory");  /* compiler barrier only */

        bytes_read = vmbus_recvpacket(stor_channel, &response,
                                      (uint32_t)sizeof(response), NULL);
        if (bytes_read >= sizeof(struct vstor_packet)) {
            storvsc_memcpy(pkt, &response, sizeof(struct vstor_packet));
            return 0;
        }
    }

    klog(LOG_ERROR, "storvsc", "Timeout waiting for response (op %u)",
         (uint64_t)pkt->operation);
    return -1;
}

/* ---- Protocol initialization ---- */

static int storvsc_negotiate(void)
{
    struct vstor_packet pkt;

    /* Step 1: BEGIN_INITIALIZATION */
    storvsc_memset(&pkt, 0, sizeof(pkt));
    pkt.operation = VSTOR_OPERATION_BEGIN_INITIALIZATION;

    if (storvsc_send_packet(&pkt) < 0) {
        klog(LOG_ERROR, "storvsc", "BEGIN_INITIALIZATION failed");
        return -1;
    }

    if (pkt.status != VSTOR_STATUS_SUCCESS) {
        klog(LOG_ERROR, "storvsc", "BEGIN_INITIALIZATION rejected (status=%u)",
             (uint64_t)pkt.status);
        return -1;
    }

    klog(LOG_DEBUG, "storvsc", "BEGIN_INITIALIZATION OK");

    /* Step 2: QUERY_PROTOCOL_VERSION (try Win10, fall back) */
    storvsc_memset(&pkt, 0, sizeof(pkt));
    pkt.operation = VSTOR_OPERATION_QUERY_PROTOCOL_VERSION;
    pkt.version.major_minor = STORVSC_VERSION_WIN10;
    pkt.version.revision = 0;

    if (storvsc_send_packet(&pkt) < 0 || pkt.status != VSTOR_STATUS_SUCCESS) {
        /* Fall back to Win8.1 */
        klog(LOG_INFO, "storvsc", "Win10 version rejected, trying Win8.1");
        storvsc_memset(&pkt, 0, sizeof(pkt));
        pkt.operation = VSTOR_OPERATION_QUERY_PROTOCOL_VERSION;
        pkt.version.major_minor = STORVSC_VERSION_WIN8_1;
        pkt.version.revision = 0;

        if (storvsc_send_packet(&pkt) < 0 ||
            pkt.status != VSTOR_STATUS_SUCCESS) {
            /* Fall back to Win8 */
            klog(LOG_INFO, "storvsc", "Win8.1 version rejected, trying Win8");
            storvsc_memset(&pkt, 0, sizeof(pkt));
            pkt.operation = VSTOR_OPERATION_QUERY_PROTOCOL_VERSION;
            pkt.version.major_minor = STORVSC_VERSION_WIN8;
            pkt.version.revision = 0;

            if (storvsc_send_packet(&pkt) < 0 ||
                pkt.status != VSTOR_STATUS_SUCCESS) {
                klog(LOG_ERROR, "storvsc",
                     "All protocol versions rejected");
                return -1;
            }
        }
    }

    klog(LOG_INFO, "storvsc", "Protocol version negotiated");

    /* Step 3: QUERY_PROPERTIES */
    storvsc_memset(&pkt, 0, sizeof(pkt));
    pkt.operation = VSTOR_OPERATION_QUERY_PROPERTIES;

    if (storvsc_send_packet(&pkt) < 0 || pkt.status != VSTOR_STATUS_SUCCESS) {
        klog(LOG_ERROR, "storvsc", "QUERY_PROPERTIES failed");
        return -1;
    }

    /* Save max_targets / max_luns for enumeration.
     * Hyper-V typically returns 0 for both (meaning "use protocol default = 1").
     * Clamp to sane values: 0 → 1, cap at 64. */
    prop_max_targets = pkt.properties.max_targets ? pkt.properties.max_targets : 1;
    prop_max_luns    = pkt.properties.max_luns    ? pkt.properties.max_luns    : 1;
    if (prop_max_targets > 64) prop_max_targets = 64;
    if (prop_max_luns    > 64) prop_max_luns    = 64;

    klog(LOG_INFO, "storvsc", "QUERY_PROPERTIES OK: max_targets=%u max_luns=%u",
         (uint64_t)prop_max_targets, (uint64_t)prop_max_luns);

    /* Step 4: END_INITIALIZATION */
    storvsc_memset(&pkt, 0, sizeof(pkt));
    pkt.operation = VSTOR_OPERATION_END_INITIALIZATION;

    if (storvsc_send_packet(&pkt) < 0 || pkt.status != VSTOR_STATUS_SUCCESS) {
        klog(LOG_ERROR, "storvsc", "END_INITIALIZATION failed (status=%u)",
             (uint64_t)pkt.status);
        return -1;
    }

    klog(LOG_INFO, "storvsc", "Initialization complete");
    return 0;
}

/* ---- SCSI command helpers ---- */

/* Send a SCSI command targeting a specific (target_id, lun) with data
 * transfer via transfer pages (GPADL). */
static int storvsc_scsi_cmd(uint8_t target, uint8_t lun,
                             uint8_t *cdb, uint8_t cdb_len,
                             uint8_t data_in, uint32_t xfer_len)
{
    struct vstor_packet pkt;
    struct vstor_packet response;
    uint32_t bytes_read;
    uint32_t attempts;
    uint64_t tid = storvsc_next_tid++;

    storvsc_memset(&pkt, 0, sizeof(pkt));
    pkt.operation = VSTOR_OPERATION_EXECUTE_SRB;
    pkt.flags     = VSTOR_FLAG_REQUEST_COMPLETION;

    pkt.srb.length              = sizeof(struct vstor_srb);
    pkt.srb.target_id           = target;
    pkt.srb.path_id             = 0;
    pkt.srb.lun                 = lun;
    pkt.srb.cdb_length          = cdb_len;
    pkt.srb.data_transfer_length = xfer_len;
    pkt.srb.data_in             = data_in;

    storvsc_memcpy(pkt.srb.cdb, cdb, cdb_len);

    if (xfer_len > 0 && xfer_buffer) {
        /* Send as transfer-page packet -- references xfer_buffer GPADL */
        struct vmbus_transfer_page_range range;
        range.byte_count  = xfer_len;
        range.byte_offset = 0;

        if (vmbus_sendpacket_pagebuffer(
                stor_channel, &pkt, (uint32_t)sizeof(pkt),
                tid, (uint16_t)xfer_gpadl_handle,
                &range, 1) < 0) {
            klog(LOG_ERROR, "storvsc", "sendpacket_pagebuffer failed for CDB 0x%x",
                 (uint64_t)cdb[0]);
            return -1;
        }
    } else {
        /* No data -- send in-band */
        if (vmbus_sendpacket(
                stor_channel, &pkt, (uint32_t)sizeof(pkt),
                tid, VMBUS_PACKET_TYPE_DATA_INBAND,
                VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED) < 0) {
            klog(LOG_ERROR, "storvsc", "sendpacket failed for CDB 0x%x",
                 (uint64_t)cdb[0]);
            return -1;
        }
    }

    /* Signal the host */
    vmbus_signal_channel(stor_channel);

    /* Poll for response (NO PAUSE -- avoids Hyper-V PLE slowdown) */
    for (attempts = 0; attempts < 5000000; attempts++) {
        __asm__ volatile("" ::: "memory");  /* compiler barrier only */

        bytes_read = vmbus_recvpacket(stor_channel, &response,
                                      (uint32_t)sizeof(response), NULL);
        if (bytes_read >= sizeof(struct vstor_packet)) {
            storvsc_memcpy(&pkt, &response, sizeof(struct vstor_packet));
            break;
        }
    }

    if (attempts >= 5000000) {
        klog(LOG_ERROR, "storvsc", "Timeout waiting for SCSI response (CDB 0x%x)",
             (uint64_t)cdb[0]);
        return -1;
    }

    if (pkt.srb.srb_status != 0x01 && pkt.srb.srb_status != 0x00) {
        /* SRB_STATUS_SUCCESS = 0x01, some hosts use 0x00 */
        klog(LOG_ERROR, "storvsc",
             "SCSI cmd 0x%x failed: srb_status=0x%x scsi_status=0x%x",
             (uint64_t)cdb[0],
             (uint64_t)pkt.srb.srb_status,
             (uint64_t)pkt.srb.scsi_status);
        return -1;
    }

    return 0;
}

/* ---- Per-LUN SCSI helpers ---- */

/* Issue SCSI INQUIRY to a specific (target, lun).
 * Returns peripheral qualifier (bits 7:5 of byte 0), or -1 on timeout. */
static int storvsc_inquiry_lun(uint8_t target, uint8_t lun)
{
    uint8_t cdb[16];

    storvsc_memset(cdb, 0, 16);
    cdb[0] = SCSI_INQUIRY;
    cdb[4] = 36;  /* allocation length */

    if (xfer_buffer)
        storvsc_memset(xfer_buffer, 0, 36);

    if (storvsc_scsi_cmd(target, lun, cdb, 6, 1, 36) < 0)
        return -1;  /* no response / timeout */

    /* Byte 0 of INQUIRY response: bits[7:5] = peripheral qualifier */
    if (!xfer_buffer)
        return SCSI_PQ_CONNECTED;  /* assume present if no buffer */

    uint8_t byte0 = ((uint8_t *)xfer_buffer)[0];
    return (byte0 >> 5) & 0x07;
}

/* Issue READ_CAPACITY(16) to a specific (target, lun).
 * Writes sector_count and sector_size into out params.
 * Returns 0 on success, -1 on failure. */
static int storvsc_capacity_lun(uint8_t target, uint8_t lun,
                                uint64_t *out_sectors, uint32_t *out_size)
{
    uint8_t cdb[16];
    uint8_t cap_data[32];

    storvsc_memset(cdb, 0, 16);
    cdb[0] = SCSI_READ_CAPACITY_16;
    cdb[1] = SCSI_SAI_READ_CAPACITY_16;
    cdb[13] = 32;  /* allocation length */

    if (xfer_buffer)
        storvsc_memset(xfer_buffer, 0, 32);

    if (storvsc_scsi_cmd(target, lun, cdb, 16, 1, 32) < 0)
        return -1;

    if (xfer_buffer)
        storvsc_memcpy(cap_data, xfer_buffer, 32);
    else
        storvsc_memset(cap_data, 0, 32);

    /* Parse last LBA (big-endian 64-bit, bytes 0-7) */
    uint64_t last_lba = 0;
    last_lba |= ((uint64_t)cap_data[0]) << 56;
    last_lba |= ((uint64_t)cap_data[1]) << 48;
    last_lba |= ((uint64_t)cap_data[2]) << 40;
    last_lba |= ((uint64_t)cap_data[3]) << 32;
    last_lba |= ((uint64_t)cap_data[4]) << 24;
    last_lba |= ((uint64_t)cap_data[5]) << 16;
    last_lba |= ((uint64_t)cap_data[6]) << 8;
    last_lba |= ((uint64_t)cap_data[7]);

    /* Parse sector size (big-endian 32-bit, bytes 8-11) */
    uint32_t sector_size = 0;
    sector_size |= ((uint32_t)cap_data[8])  << 24;
    sector_size |= ((uint32_t)cap_data[9])  << 16;
    sector_size |= ((uint32_t)cap_data[10]) << 8;
    sector_size |= ((uint32_t)cap_data[11]);

    if (sector_size == 0)
        sector_size = 512;
    if (last_lba == 0)
        last_lba = (128ULL * 1024 * 1024 / sector_size) - 1;

    *out_sectors = last_lba + 1;
    *out_size    = sector_size;
    return 0;
}

/* ---- Full LUN enumeration ---- */

/* storvsc_enumerate() — discover all (target, lun) pairs.
 * Uses max_targets / max_luns from QUERY_PROPERTIES (defaults to 1/1).
 * For each responding LUN, issues READ_CAPACITY and registers a blkdev. */
static int storvsc_enumerate(void)
{
    uint8_t max_targets = (prop_max_targets > 0) ? prop_max_targets : 1;
    uint8_t max_luns    = (prop_max_luns    > 0) ? prop_max_luns    : 1;
    uint8_t t, l;
    int pq;

    klog(LOG_INFO, "storvsc", "Enumerating %u targets × %u LUNs",
         (uint64_t)max_targets, (uint64_t)max_luns);

    for (t = 0; t < max_targets; t++) {
        for (l = 0; l < max_luns; l++) {

            if (disk_count >= STORVSC_MAX_DEVICES)
                break;

            pq = storvsc_inquiry_lun(t, l);

            if (pq < 0) {
                /* Timeout — target likely doesn't exist, stop scanning this target */
                break;
            }

            if (pq == SCSI_PQ_NOT_SUPPORTED) {
                /* LUN not reachable — skip */
                continue;
            }

            if (pq == SCSI_PQ_DISCONNECTED) {
                klog(LOG_INFO, "storvsc",
                     "Target %u LUN %u: supported but not connected -- skipping",
                     (uint64_t)t, (uint64_t)l);
                continue;
            }

            /* pq == SCSI_PQ_CONNECTED (0): device present */
            uint64_t sectors = 0;
            uint32_t sector_sz = 512;

            if (storvsc_capacity_lun(t, l, &sectors, &sector_sz) < 0) {
                klog(LOG_WARN, "storvsc",
                     "Target %u LUN %u: READ_CAPACITY failed -- skipping",
                     (uint64_t)t, (uint64_t)l);
                continue;
            }

            int idx = disk_count;
            disk_info[idx].target_id    = t;
            disk_info[idx].lun          = l;
            disk_info[idx].sector_count = sectors;
            disk_info[idx].sector_size  = sector_sz;
            disk_info[idx].active       = 1;

            klog(LOG_INFO, "storvsc",
                 "Target %u LUN %u: %llu sectors, %u bytes/sector",
                 (uint64_t)t, (uint64_t)l,
                 sectors, (uint64_t)sector_sz);

            /* Build blkdev name: "hyperv" + decimal index */
            struct blkdev dev;
            storvsc_memset(&dev, 0, sizeof(dev));
            dev.name[0] = 'h'; dev.name[1] = 'y'; dev.name[2] = 'p';
            dev.name[3] = 'e'; dev.name[4] = 'r'; dev.name[5] = 'v';
            /* Simple index → ASCII digit(s) (supports 0-99) */
            if (idx < 10) {
                dev.name[6] = '0' + idx;
                dev.name[7] = '\0';
            } else {
                dev.name[6] = '0' + (idx / 10);
                dev.name[7] = '0' + (idx % 10);
                dev.name[8] = '\0';
            }

            dev.sector_size  = sector_sz;
            dev.sector_count = sectors;
            dev.read         = storvsc_blk_read;
            dev.write        = storvsc_blk_write;
            dev.driver_data  = (void *)(uintptr_t)idx;
            dev.active       = 1;

            if (blkdev_register(&dev) < 0) {
                klog(LOG_ERROR, "storvsc",
                     "Failed to register blkdev hyperv%d", (uint64_t)idx);
            } else {
                printk("[OK] StorVSC: hyperv%d (T%u L%u, %u sectors, %u bytes/sector)\n",
                       idx, (uint32_t)t, (uint32_t)l,
                       (uint32_t)sectors, sector_sz);
                disk_count++;
            }
        }
    }

    return disk_count > 0 ? 0 : -1;
}

/* ---- Block device callbacks ---- */

static int storvsc_blk_read(uint64_t lba, uint32_t count, void *buf,
                             void *driver_data)
{
    uint8_t cdb[16];
    uint32_t sectors_per_chunk;
    uint32_t chunk;
    uint32_t remaining = count;
    uint64_t cur_lba = lba;
    uint8_t *dst = (uint8_t *)buf;
    int idx = (int)(uintptr_t)driver_data;

    if (!storvsc_initialized || !xfer_buffer)
        return -1;
    if (idx < 0 || idx >= disk_count || !disk_info[idx].active)
        return -1;

    /* Calculate how many sectors fit in our transfer buffer */
    sectors_per_chunk = (STORVSC_XFER_PAGES * 4096) / disk_info[idx].sector_size;

    while (remaining > 0) {
        chunk = remaining;
        if (chunk > sectors_per_chunk)
            chunk = sectors_per_chunk;

        uint32_t byte_count = chunk * disk_info[idx].sector_size;

        /* Build SCSI READ(16) CDB */
        storvsc_memset(cdb, 0, 16);
        cdb[0] = SCSI_READ_16;

        /* LBA (bytes 2-9, big-endian) */
        cdb[2]  = (uint8_t)(cur_lba >> 56);
        cdb[3]  = (uint8_t)(cur_lba >> 48);
        cdb[4]  = (uint8_t)(cur_lba >> 40);
        cdb[5]  = (uint8_t)(cur_lba >> 32);
        cdb[6]  = (uint8_t)(cur_lba >> 24);
        cdb[7]  = (uint8_t)(cur_lba >> 16);
        cdb[8]  = (uint8_t)(cur_lba >> 8);
        cdb[9]  = (uint8_t)(cur_lba);

        /* Transfer length in sectors (bytes 10-13, big-endian) */
        cdb[10] = (uint8_t)(chunk >> 24);
        cdb[11] = (uint8_t)(chunk >> 16);
        cdb[12] = (uint8_t)(chunk >> 8);
        cdb[13] = (uint8_t)(chunk);

        if (storvsc_scsi_cmd(disk_info[idx].target_id, disk_info[idx].lun,
                             cdb, 16, 1, byte_count) < 0)
            return -1;

        /* Copy from transfer buffer to caller's buffer */
        storvsc_memcpy(dst, xfer_buffer, byte_count);

        dst       += byte_count;
        cur_lba   += chunk;
        remaining -= chunk;
    }

    return 0;
}

static int storvsc_blk_write(uint64_t lba, uint32_t count, const void *buf,
                              void *driver_data)
{
    uint8_t cdb[16];
    uint32_t sectors_per_chunk;
    uint32_t chunk;
    uint32_t remaining = count;
    uint64_t cur_lba = lba;
    const uint8_t *src = (const uint8_t *)buf;
    int idx = (int)(uintptr_t)driver_data;

    if (!storvsc_initialized || !xfer_buffer)
        return -1;
    if (idx < 0 || idx >= disk_count || !disk_info[idx].active)
        return -1;

    sectors_per_chunk = (STORVSC_XFER_PAGES * 4096) / disk_info[idx].sector_size;

    while (remaining > 0) {
        chunk = remaining;
        if (chunk > sectors_per_chunk)
            chunk = sectors_per_chunk;

        uint32_t byte_count = chunk * disk_info[idx].sector_size;

        /* Copy caller's data into transfer buffer */
        storvsc_memcpy(xfer_buffer, src, byte_count);

        /* Build SCSI WRITE(16) CDB */
        storvsc_memset(cdb, 0, 16);
        cdb[0] = SCSI_WRITE_16;

        /* LBA (bytes 2-9, big-endian) */
        cdb[2]  = (uint8_t)(cur_lba >> 56);
        cdb[3]  = (uint8_t)(cur_lba >> 48);
        cdb[4]  = (uint8_t)(cur_lba >> 40);
        cdb[5]  = (uint8_t)(cur_lba >> 32);
        cdb[6]  = (uint8_t)(cur_lba >> 24);
        cdb[7]  = (uint8_t)(cur_lba >> 16);
        cdb[8]  = (uint8_t)(cur_lba >> 8);
        cdb[9]  = (uint8_t)(cur_lba);

        /* Transfer length in sectors (bytes 10-13, big-endian) */
        cdb[10] = (uint8_t)(chunk >> 24);
        cdb[11] = (uint8_t)(chunk >> 16);
        cdb[12] = (uint8_t)(chunk >> 8);
        cdb[13] = (uint8_t)(chunk);

        if (storvsc_scsi_cmd(disk_info[idx].target_id, disk_info[idx].lun,
                             cdb, 16, 0, byte_count) < 0)
            return -1;

        src       += byte_count;
        cur_lba   += chunk;
        remaining -= chunk;
    }

    return 0;
}

/* ---- Public API ---- */

int storvsc_init(void)
{
    struct hv_guid stor_guid = HV_GUID_STORVSC_CHANNEL;

    /* Only runs on Hyper-V after VMBus is initialized */
    if (vmbus_get_channel_count() <= 0) {
        klog(LOG_DEBUG, "storvsc", "No VMBus channels -- skipping StorVSC");
        return -1;
    }

    /* Find the Storage VSP channel */
    stor_channel = vmbus_find_channel_by_guid(&stor_guid);
    if (!stor_channel) {
        klog(LOG_WARN, "storvsc", "Storage VSP channel not found");
        return -1;
    }

    klog(LOG_INFO, "storvsc",
         "Found Storage VSP channel (relid=%u)",
         (uint64_t)stor_channel->child_relid);

    /* Open the channel with ring buffers */
    if (vmbus_open_channel(stor_channel, STORVSC_RING_PAGES) < 0) {
        klog(LOG_ERROR, "storvsc", "Failed to open storage channel");
        return -1;
    }

    /* Allocate transfer buffer (PMM -- identity-mapped, page-aligned). */
    xfer_buffer = (void *)(uintptr_t)pmm_alloc_contiguous(STORVSC_XFER_PAGES);
    if (!xfer_buffer) {
        klog(LOG_ERROR, "storvsc",
             "Failed to allocate transfer buffer (%u pages)",
             (uint64_t)STORVSC_XFER_PAGES);
        return -1;
    }
    if ((uintptr_t)xfer_buffer & 0xFFF) {
        klog(LOG_ERROR, "storvsc",
             "Transfer buffer at 0x%x is NOT page-aligned",
             (uint64_t)(uintptr_t)xfer_buffer);
        return -1;
    }
    storvsc_memset(xfer_buffer, 0, STORVSC_XFER_PAGES * 4096);

    /* Create GPADL for the transfer buffer */
    if (vmbus_create_gpadl_external(stor_channel, xfer_buffer,
                                     STORVSC_XFER_PAGES,
                                     &xfer_gpadl_handle) < 0) {
        klog(LOG_ERROR, "storvsc", "Failed to create transfer buffer GPADL");
        return -1;
    }
    klog(LOG_INFO, "storvsc", "Transfer buffer GPADL=0x%x (%u pages at 0x%x)",
         (uint64_t)xfer_gpadl_handle, (uint64_t)STORVSC_XFER_PAGES,
         (uint64_t)(uintptr_t)xfer_buffer);

    /* Negotiate StorVSC protocol */
    if (storvsc_negotiate() < 0) {
        klog(LOG_ERROR, "storvsc", "Protocol negotiation failed");
        return -1;
    }

    /* Enumerate all LUNs and register block devices */
    if (storvsc_enumerate() < 0) {
        klog(LOG_ERROR, "storvsc", "No storage devices found");
        return -1;
    }

    storvsc_initialized = 1;
    klog(LOG_INFO, "storvsc", "%d device(s) registered", (uint64_t)disk_count);
    return 0;
}
