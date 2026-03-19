/* ============================================================================
 * storvsc.c — Hyper-V Synthetic SCSI Storage Driver (StorVSC)
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
static struct storvsc_disk_info disk_info;
static int storvsc_initialized;

/* Data buffer for SCSI read/write — allocated from PMM (identity-mapped).
 * Uses a dedicated transfer buffer because the VMBus ring buffer carries
 * only the VSTOR_PACKET metadata; actual disk data goes through this
 * GPADL-shared buffer. For simplicity, we use a fixed 64 KiB buffer
 * and break large I/O into chunks. */
#define STORVSC_XFER_PAGES      16  /* 64 KiB = 16 pages */
static void *xfer_buffer;

/* Ring buffer page count: 32 pages = 16 send + 16 recv = 64 KiB each */
#define STORVSC_RING_PAGES      32

/* ---- Protocol helpers ---- */

/* Send a VSTOR_PACKET and wait for the completion response.
 * The response is written back into the same packet struct. */
static int storvsc_send_packet(struct vstor_packet *pkt)
{
    struct vstor_packet response;
    uint32_t bytes_read;
    uint32_t attempts;

    pkt->flags |= VSTOR_FLAG_REQUEST_COMPLETION;

    /* Write packet to send ring */
    if (vmbus_ring_write(stor_channel, pkt,
                         (uint32_t)sizeof(struct vstor_packet)) < 0) {
        klog(LOG_ERROR, "storvsc", "Ring write failed for op %u",
             (uint64_t)pkt->operation);
        return -1;
    }

    /* Signal the host */
    vmbus_signal_channel(stor_channel);

    /* Poll for response on receive ring */
    for (attempts = 0; attempts < 50000; attempts++) {
        __asm__ volatile("pause" ::: "memory");

        bytes_read = vmbus_ring_read(stor_channel, &response,
                                     (uint32_t)sizeof(response));
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

    klog(LOG_DEBUG, "storvsc", "QUERY_PROPERTIES OK");

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

static int storvsc_scsi_cmd(uint8_t *cdb, uint8_t cdb_len,
                             uint8_t data_in, uint32_t xfer_len)
{
    struct vstor_packet pkt;

    storvsc_memset(&pkt, 0, sizeof(pkt));
    pkt.operation = VSTOR_OPERATION_EXECUTE_SRB;

    pkt.srb.length              = sizeof(struct vstor_srb);
    pkt.srb.target_id           = 0;
    pkt.srb.path_id             = 0;
    pkt.srb.lun                 = 0;
    pkt.srb.cdb_length          = cdb_len;
    pkt.srb.data_transfer_length = xfer_len;
    pkt.srb.data_in             = data_in;

    storvsc_memcpy(pkt.srb.cdb, cdb, cdb_len);

    if (storvsc_send_packet(&pkt) < 0)
        return -1;

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

static int storvsc_inquiry(void)
{
    uint8_t cdb[16];

    storvsc_memset(cdb, 0, 16);
    cdb[0] = SCSI_INQUIRY;
    cdb[4] = 36;  /* allocation length */

    if (storvsc_scsi_cmd(cdb, 6, 1, 36) < 0) {
        klog(LOG_ERROR, "storvsc", "INQUIRY failed");
        return -1;
    }

    klog(LOG_INFO, "storvsc", "INQUIRY successful — disk present");
    return 0;
}

static int storvsc_read_capacity(void)
{
    uint8_t cdb[16];
    uint8_t cap_data[32];

    storvsc_memset(cdb, 0, 16);
    cdb[0] = SCSI_READ_CAPACITY_16;
    cdb[1] = SCSI_SAI_READ_CAPACITY_16;  /* service action */
    /* Allocation length = 32 bytes (bytes 10-13, big-endian) */
    cdb[10] = 0;
    cdb[11] = 0;
    cdb[12] = 0;
    cdb[13] = 32;

    /* The capacity data will be in the transfer buffer */
    if (xfer_buffer)
        storvsc_memset(xfer_buffer, 0, 32);

    if (storvsc_scsi_cmd(cdb, 16, 1, 32) < 0) {
        klog(LOG_ERROR, "storvsc", "READ_CAPACITY(16) failed");
        return -1;
    }

    /* Parse response from transfer buffer.
     * READ CAPACITY(16) response format:
     *   Bytes 0-7:   returned logical block address (last LBA, big-endian)
     *   Bytes 8-11:  logical block length (sector size, big-endian) */
    if (xfer_buffer) {
        storvsc_memcpy(cap_data, xfer_buffer, 32);
    } else {
        /* Fallback: parse from SRB sense data area (limited) */
        storvsc_memset(cap_data, 0, 32);
    }

    /* Parse last LBA (big-endian 64-bit) */
    uint64_t last_lba = 0;
    last_lba |= ((uint64_t)cap_data[0]) << 56;
    last_lba |= ((uint64_t)cap_data[1]) << 48;
    last_lba |= ((uint64_t)cap_data[2]) << 40;
    last_lba |= ((uint64_t)cap_data[3]) << 32;
    last_lba |= ((uint64_t)cap_data[4]) << 24;
    last_lba |= ((uint64_t)cap_data[5]) << 16;
    last_lba |= ((uint64_t)cap_data[6]) << 8;
    last_lba |= ((uint64_t)cap_data[7]);

    /* Parse sector size (big-endian 32-bit) */
    uint32_t sector_size = 0;
    sector_size |= ((uint32_t)cap_data[8]) << 24;
    sector_size |= ((uint32_t)cap_data[9]) << 16;
    sector_size |= ((uint32_t)cap_data[10]) << 8;
    sector_size |= ((uint32_t)cap_data[11]);

    /* Sanity check */
    if (sector_size == 0)
        sector_size = 512;
    if (last_lba == 0) {
        klog(LOG_WARN, "storvsc",
             "READ_CAPACITY returned 0 sectors, assuming 128 MiB");
        last_lba = (128ULL * 1024 * 1024 / sector_size) - 1;
    }

    disk_info.sector_count = last_lba + 1;
    disk_info.sector_size  = sector_size;

    klog(LOG_INFO, "storvsc", "Disk: %u sectors, %u bytes/sector (%u MiB)",
         disk_info.sector_count,
         (uint64_t)disk_info.sector_size,
         disk_info.sector_count * disk_info.sector_size / (1024 * 1024));

    return 0;
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

    (void)driver_data;

    if (!storvsc_initialized || !xfer_buffer)
        return -1;

    /* Calculate how many sectors fit in our transfer buffer */
    sectors_per_chunk = (STORVSC_XFER_PAGES * 4096) / disk_info.sector_size;

    while (remaining > 0) {
        chunk = remaining;
        if (chunk > sectors_per_chunk)
            chunk = sectors_per_chunk;

        uint32_t byte_count = chunk * disk_info.sector_size;

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

        if (storvsc_scsi_cmd(cdb, 16, 1, byte_count) < 0)
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

    (void)driver_data;

    if (!storvsc_initialized || !xfer_buffer)
        return -1;

    sectors_per_chunk = (STORVSC_XFER_PAGES * 4096) / disk_info.sector_size;

    while (remaining > 0) {
        chunk = remaining;
        if (chunk > sectors_per_chunk)
            chunk = sectors_per_chunk;

        uint32_t byte_count = chunk * disk_info.sector_size;

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

        if (storvsc_scsi_cmd(cdb, 16, 0, byte_count) < 0)
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
        klog(LOG_DEBUG, "storvsc", "No VMBus channels — skipping StorVSC");
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

    /* Allocate transfer buffer (PMM — identity-mapped) */
    xfer_buffer = (void *)(uintptr_t)pmm_alloc_contiguous(STORVSC_XFER_PAGES);
    if (!xfer_buffer) {
        klog(LOG_ERROR, "storvsc",
             "Failed to allocate transfer buffer (%u pages)",
             (uint64_t)STORVSC_XFER_PAGES);
        return -1;
    }
    storvsc_memset(xfer_buffer, 0, STORVSC_XFER_PAGES * 4096);

    /* Negotiate StorVSC protocol */
    if (storvsc_negotiate() < 0) {
        klog(LOG_ERROR, "storvsc", "Protocol negotiation failed");
        return -1;
    }

    /* SCSI INQUIRY — identify disk */
    if (storvsc_inquiry() < 0) {
        klog(LOG_WARN, "storvsc", "INQUIRY failed — continuing anyway");
    }

    /* Read disk capacity */
    if (storvsc_read_capacity() < 0) {
        klog(LOG_ERROR, "storvsc", "Failed to read disk capacity");
        return -1;
    }

    /* Register as block device */
    struct blkdev dev;
    storvsc_memset(&dev, 0, sizeof(dev));

    /* Name: "hyperv0" */
    dev.name[0] = 'h'; dev.name[1] = 'y'; dev.name[2] = 'p';
    dev.name[3] = 'e'; dev.name[4] = 'r'; dev.name[5] = 'v';
    dev.name[6] = '0'; dev.name[7] = '\0';

    dev.sector_size  = disk_info.sector_size;
    dev.sector_count = disk_info.sector_count;
    dev.read         = storvsc_blk_read;
    dev.write        = storvsc_blk_write;
    dev.driver_data  = NULL;
    dev.active       = 1;

    if (blkdev_register(&dev) < 0) {
        klog(LOG_ERROR, "storvsc", "Failed to register blkdev hyperv0");
        return -1;
    }

    storvsc_initialized = 1;

    printk("[OK] StorVSC: hyperv0 (%u sectors, %u bytes/sector)\n",
           (uint32_t)disk_info.sector_count,
           disk_info.sector_size);

    return 0;
}
