/* ============================================================================
 * usb_msc.c -- USB Mass Storage Class (Bulk-Only Transport) driver
 *
 * Implements SCSI-over-USB transport: CBW/CSW framing around SCSI commands.
 * Supports INQUIRY, READ CAPACITY(10), READ(10), WRITE(10).
 *
 * Reference: USB Mass Storage Class Bulk-Only Transport 1.0
 *            SCSI Primary Commands (SPC-4), SCSI Block Commands (SBC-3)
 * ============================================================================ */

#include "kernel/drivers/usb_msc.h"
#include "kernel/drivers/xhci.h"
#include "kernel/drivers/xhci_dev.h"
#include "kernel/drivers/xhci_ring.h"
#include "kernel/mm/pmm.h"
#include "kernel/klog.h"

/* ---- Static state ---- */

/* Per-command transfer cap: one xHCI Normal TRB carries at most 64 KiB
 * (17-bit TRB Transfer Length). READ(10)/WRITE(10) further cap the block
 * count at 16 bits; both bounds are enforced by chunking in the sector ops. */
#define MSC_MAX_XFER_BYTES   65536u

static struct usb_msc_info msc_info[XHCI_MAX_DEVICES];
static uint32_t cbw_tag = 1;

/* ---- Helpers ---- */

static void msc_zero(void *dst, uint64_t bytes)
{
    uint8_t *p = (uint8_t *)dst;
    uint64_t i;
    for (i = 0; i < bytes; i++) p[i] = 0;
}

static void msc_copy(void *dst, const void *src, uint64_t bytes)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint64_t i;
    for (i = 0; i < bytes; i++) d[i] = s[i];
}

static void msc_delay_us(uint32_t us)
{
    uint32_t i;
    for (i = 0; i < us; i++)
        __asm__ volatile("outb %%al, $0x80" ::: "memory");
}

/* ---- BOT transport ---- */

/* Execute a BOT command: send CBW, optional data phase, receive CSW.
 * Returns CSW status (0=pass, 1=fail, 2=phase error) or -1 on transport error. */
static int msc_bot_command(struct xhci_controller *hc, struct xhci_device *dev,
                           const uint8_t *cdb, uint8_t cdb_len,
                           void *data, uint32_t data_len, int dir_in)
{
    struct usb_cbw cbw;
    struct usb_csw csw;
    uint32_t tag = cbw_tag++;

    /* Build CBW */
    msc_zero(&cbw, sizeof(cbw));
    cbw.dCBWSignature = USB_MSC_CBW_SIGNATURE;
    cbw.dCBWTag = tag;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags = dir_in ? USB_CBW_FLAG_IN : USB_CBW_FLAG_OUT;
    /* Address the device's currently-selected LUN (0 unless boot-LUN selection
     * picked another for composite media like card readers). */
    cbw.bCBWLUN = msc_info[dev->slot_id].current_lun;
    cbw.bCBWCBLength = cdb_len;
    msc_copy(cbw.CBWCB, cdb, cdb_len);

    /* Send CBW via Bulk-OUT */
    if (xhci_bulk_transfer(hc, dev, &dev->bulk_out_ring,
                           &cbw, USB_MSC_CBW_SIZE, 0) != 0) {
        klog(LOG_ERROR, "usb-msc", "CBW send failed");
        return -1;
    }

    /* Data phase (optional) */
    if (data && data_len > 0) {
        struct xhci_ring *ring = dir_in ? &dev->bulk_in_ring : &dev->bulk_out_ring;
        if (xhci_bulk_transfer(hc, dev, ring, data, data_len, dir_in) != 0) {
            klog(LOG_ERROR, "usb-msc", "Data phase failed (%s, %u bytes)",
                 dir_in ? "IN" : "OUT", (uint64_t)data_len);
            return -1;
        }
    }

    /* Receive CSW via Bulk-IN */
    msc_zero(&csw, sizeof(csw));
    if (xhci_bulk_transfer(hc, dev, &dev->bulk_in_ring,
                           &csw, USB_MSC_CSW_SIZE, 1) != 0) {
        klog(LOG_ERROR, "usb-msc", "CSW receive failed");
        return -1;
    }

    /* Validate CSW */
    if (csw.dCSWSignature != USB_MSC_CSW_SIGNATURE) {
        klog(LOG_ERROR, "usb-msc", "CSW bad signature: 0x%08x",
             (uint64_t)csw.dCSWSignature);
        return -1;
    }
    if (csw.dCSWTag != tag) {
        klog(LOG_ERROR, "usb-msc", "CSW tag mismatch: got %u, expected %u",
             (uint64_t)csw.dCSWTag, (uint64_t)tag);
        return -1;
    }

    return csw.bCSWStatus;
}

/* ---- SCSI commands ---- */

static int msc_test_unit_ready(struct xhci_controller *hc, struct xhci_device *dev)
{
    uint8_t cdb[6];
    msc_zero(cdb, 6);
    cdb[0] = SCSI_TEST_UNIT_READY;
    return msc_bot_command(hc, dev, cdb, 6, NULL, 0, 0);
}

/* ---- SCSI REQUEST SENSE + error classification ---- */

const char *msc_sense_key_name(uint8_t key)
{
    switch (key & 0x0F) {
    case SCSI_SK_NO_SENSE:        return "NO SENSE";
    case SCSI_SK_RECOVERED:       return "RECOVERED";
    case SCSI_SK_NOT_READY:       return "NOT READY";
    case SCSI_SK_MEDIUM_ERROR:    return "MEDIUM ERROR";
    case SCSI_SK_HARDWARE_ERROR:  return "HARDWARE ERROR";
    case SCSI_SK_ILLEGAL_REQUEST: return "ILLEGAL REQUEST";
    case SCSI_SK_UNIT_ATTENTION:  return "UNIT ATTENTION";
    case SCSI_SK_DATA_PROTECT:    return "DATA PROTECT";
    case SCSI_SK_BLANK_CHECK:     return "BLANK CHECK";
    case SCSI_SK_ABORTED_COMMAND: return "ABORTED COMMAND";
    default:                      return "OTHER";
    }
}

msc_err_class_t msc_sense_classify(const uint8_t *sense)
{
    if (!sense)
        return MSC_ERR_UNRECOVERABLE;
    /* Require a valid fixed-format response code (0x70 current / 0x71 deferred,
     * with the top bit the VALID flag). A short or zero-filled REQUEST SENSE
     * reply -- xhci_bulk_transfer reports SHORT_PKT as success without a byte
     * count -- would otherwise misread as key 0 (NO SENSE / OK). */
    if ((sense[0] & 0x7F) != 0x70 && (sense[0] & 0x7F) != 0x71)
        return MSC_ERR_UNRECOVERABLE;
    switch (sense[2] & 0x0F) {
    case SCSI_SK_NO_SENSE:
    case SCSI_SK_RECOVERED:
        return MSC_ERR_OK;
    case SCSI_SK_UNIT_ATTENTION:
        return MSC_ERR_RETRY_NOW;   /* device reset itself; retry immediately */
    case SCSI_SK_NOT_READY:
        return MSC_ERR_WAIT_RETRY;  /* spinning up; wait + retry */
    default:
        return MSC_ERR_UNRECOVERABLE;
    }
}

/* Issue REQUEST SENSE (fixed format, 18 bytes) after a failed command and log
 * the decoded key/ASC/ASCQ. Per SPC, REQUEST SENSE itself does not raise a
 * CHECK CONDITION, so there is no recursion: a transport failure here is just
 * reported. Fills sense[SCSI_SENSE_LEN]; returns the classified error class
 * (MSC_ERR_UNRECOVERABLE if the sense request itself failed). */
static msc_err_class_t msc_request_sense(struct xhci_controller *hc,
                                         struct xhci_device *dev, uint8_t *sense)
{
    uint8_t cdb[6];
    int rc;

    msc_zero(cdb, 6);
    cdb[0] = SCSI_REQUEST_SENSE;
    /* cdb[1] bit 0 (DESC) stays 0 -> request FIXED-format sense (response code
     * 0x70/0x71). A conformant target must not return descriptor format (0x72/
     * 0x73) when DESC=0 (SPC-4 6.27), so the fixed-format-only parse is correct
     * by contract for boot MSC. */
    cdb[4] = SCSI_SENSE_LEN;          /* allocation length */
    msc_zero(sense, SCSI_SENSE_LEN);

    rc = msc_bot_command(hc, dev, cdb, 6, sense, SCSI_SENSE_LEN, 1);
    if (rc != USB_CSW_STATUS_PASS) {
        klog(LOG_WARN, "usb-msc", "REQUEST SENSE failed (status=%d)", (uint64_t)rc);
        return MSC_ERR_UNRECOVERABLE;
    }
    klog(LOG_INFO, "usb-msc", "Sense: key=%u ASC=0x%02x ASCQ=0x%02x (%s)",
         (uint64_t)(sense[2] & 0x0F), (uint64_t)sense[12], (uint64_t)sense[13],
         msc_sense_key_name(sense[2]));
    return msc_sense_classify(sense);
}

static int msc_inquiry(struct xhci_controller *hc, struct xhci_device *dev,
                       struct usb_msc_info *info)
{
    uint8_t cdb[6];
    uint8_t buf[36];
    int rc;

    msc_zero(cdb, 6);
    cdb[0] = SCSI_INQUIRY;
    cdb[4] = 36;  /* Allocation length */

    msc_zero(buf, 36);
    rc = msc_bot_command(hc, dev, cdb, 6, buf, 36, 1);
    if (rc != USB_CSW_STATUS_PASS) {
        klog(LOG_ERROR, "usb-msc", "INQUIRY failed (status=%d)", (uint64_t)rc);
        return -1;
    }

    info->device_type = buf[0] & 0x1F;

    /* Vendor: bytes 8-15 (8 chars, space-padded) */
    msc_copy(info->vendor, buf + 8, 8);
    info->vendor[8] = '\0';

    /* Product: bytes 16-31 (16 chars, space-padded) */
    msc_copy(info->product, buf + 16, 16);
    info->product[16] = '\0';

    /* Trim trailing spaces */
    {
        int i;
        for (i = 7; i >= 0 && info->vendor[i] == ' '; i--)
            info->vendor[i] = '\0';
        for (i = 15; i >= 0 && info->product[i] == ' '; i--)
            info->product[i] = '\0';
    }

    klog(LOG_INFO, "usb-msc", "INQUIRY: type=%u, \"%s\" \"%s\"",
         (uint64_t)info->device_type,
         info->vendor, info->product);

    return 0;
}

static int msc_read_capacity(struct xhci_controller *hc, struct xhci_device *dev,
                             struct usb_msc_info *info)
{
    uint8_t cdb[10];
    uint8_t buf[8];
    int rc;

    msc_zero(cdb, 10);
    cdb[0] = SCSI_READ_CAPACITY_10;

    msc_zero(buf, 8);
    rc = msc_bot_command(hc, dev, cdb, 10, buf, 8, 1);
    if (rc != USB_CSW_STATUS_PASS) {
        klog(LOG_ERROR, "usb-msc", "READ CAPACITY failed (status=%d)", (uint64_t)rc);
        return -1;
    }

    /* Response is big-endian */
    uint32_t last_lba = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                        ((uint32_t)buf[2] << 8)  | (uint32_t)buf[3];
    uint32_t block_size = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                          ((uint32_t)buf[6] << 8)  | (uint32_t)buf[7];

    /* Validate device-reported geometry before trusting it. last_lba ==
     * 0xFFFFFFFF means the medium needs READ CAPACITY(16) (unsupported here);
     * block_size must be a sane power-of-two sector size so downstream
     * count * sector_size math and the chunking loop stay bounded. */
    if (last_lba == 0xFFFFFFFFu) {
        klog(LOG_ERROR, "usb-msc", "capacity exceeds 32-bit LBA -- unsupported");
        return -1;
    }
    if (block_size != 512 && block_size != 1024 &&
        block_size != 2048 && block_size != 4096) {
        klog(LOG_ERROR, "usb-msc", "unsupported sector size %u",
             (uint64_t)block_size);
        return -1;
    }

    info->sector_count = last_lba + 1;
    info->sector_size = block_size;
    info->capacity_bytes = (uint64_t)info->sector_count * info->sector_size;

    uint32_t mb = (uint32_t)(info->capacity_bytes / (1024 * 1024));
    klog(LOG_INFO, "usb-msc", "READ CAPACITY: %u sectors x %u bytes = %u MiB",
         (uint64_t)info->sector_count, (uint64_t)info->sector_size, (uint64_t)mb);

    return 0;
}

/* ---- Public API ---- */

int usb_msc_init(struct xhci_controller *hc, struct xhci_device *dev)
{
    struct usb_msc_info *info;
    int rc, retries;

    if (!dev->is_msc) {
        klog(LOG_DEBUG, "usb-msc", "Slot %u is not MSC", (uint64_t)dev->slot_id);
        return -1;
    }

    if (dev->slot_id >= XHCI_MAX_DEVICES)
        return -1;

    info = &msc_info[dev->slot_id];
    msc_zero(info, sizeof(*info));

    /* TEST UNIT READY -- decode the failure via REQUEST SENSE and let the sense
     * class drive the retry: UNIT ATTENTION (post-plug reset) retries at once,
     * NOT READY (spin-up) waits, anything else is unrecoverable. */
    for (retries = 0; retries < 3; retries++) {
        rc = msc_test_unit_ready(hc, dev);
        if (rc == USB_CSW_STATUS_PASS)
            break;
        /* REQUEST SENSE is only meaningful for a SCSI command FAILURE (CSW
         * status 1). A phase error (2), transport failure (<0), or any
         * out-of-range status means the BOT pipe is desynced -- sending another
         * CBW would compound it; that path needs BOT mass-storage reset
         * (a later transport-recovery section), so stop here. */
        if (rc != USB_CSW_STATUS_FAIL)
            break;
        {
            uint8_t sense[SCSI_SENSE_LEN];
            msc_err_class_t cls = msc_request_sense(hc, dev, sense);
            if (cls == MSC_ERR_UNRECOVERABLE)
                break;         /* MEDIUM/HARDWARE error -- retrying won't help */
            if (cls == MSC_ERR_WAIT_RETRY)
                msc_delay_us(100000);  /* 100ms -- drive spinning up */
            /* MSC_ERR_RETRY_NOW (UNIT ATTENTION): loop again immediately */
        }
    }

    if (rc != USB_CSW_STATUS_PASS)
        klog(LOG_WARN, "usb-msc", "TEST UNIT READY failed (status=%d), continuing",
             (uint64_t)rc);

    /* INQUIRY */
    if (msc_inquiry(hc, dev, info) != 0)
        return -1;

    /* READ CAPACITY */
    if (msc_read_capacity(hc, dev, info) != 0)
        return -1;

    info->valid = 1;

    klog(LOG_INFO, "usb-msc",
         "USB disk ready: \"%s %s\" (%u MiB, %u-byte sectors)",
         info->vendor, info->product,
         (uint64_t)(info->capacity_bytes / (1024 * 1024)),
         (uint64_t)info->sector_size);

    return 0;
}

int usb_msc_read_sectors(struct xhci_controller *hc, struct xhci_device *dev,
                         uint32_t lba, uint32_t count, void *buf)
{
    struct usb_msc_info *info;
    uint8_t *out = (uint8_t *)buf;
    uint32_t max_blocks;

    if (dev->slot_id >= XHCI_MAX_DEVICES)
        return -1;
    info = &msc_info[dev->slot_id];
    if (!info->valid || info->sector_size == 0)
        return -1;

    if (count == 0)
        return 0;
    /* The chunk loop advances a 32-bit LBA; reject out-of-range requests so a
     * count past end-of-device cannot wrap back to low sectors. */
    if (lba >= info->sector_count || count > info->sector_count - lba)
        return -1;

    /* A single xHCI Normal TRB carries at most 64 KiB (17-bit TRB Transfer
     * Length) and READ(10) caps the block count at 16 bits; chunk the request
     * so neither the TRB length field nor cdb[7..8] overflows. */
    max_blocks = MSC_MAX_XFER_BYTES / info->sector_size;
    if (max_blocks == 0)
        max_blocks = 1;
    if (max_blocks > 0xFFFF)
        max_blocks = 0xFFFF;

    while (count > 0) {
        uint32_t chunk = count < max_blocks ? count : max_blocks;
        uint32_t data_len = chunk * info->sector_size;
        uint8_t cdb[10];
        int rc;

        msc_zero(cdb, 10);
        cdb[0] = SCSI_READ_10;
        /* LBA (big-endian) */
        cdb[2] = (uint8_t)(lba >> 24);
        cdb[3] = (uint8_t)(lba >> 16);
        cdb[4] = (uint8_t)(lba >> 8);
        cdb[5] = (uint8_t)(lba);
        /* Transfer length in sectors (big-endian) */
        cdb[7] = (uint8_t)(chunk >> 8);
        cdb[8] = (uint8_t)(chunk);

        rc = msc_bot_command(hc, dev, cdb, 10, out, data_len, 1);
        if (rc != USB_CSW_STATUS_PASS) {
            klog(LOG_ERROR, "usb-msc", "READ(10) failed: LBA=%u count=%u status=%d",
                 (uint64_t)lba, (uint64_t)chunk, (uint64_t)rc);
            return -1;
        }

        lba += chunk;
        out += data_len;
        count -= chunk;
    }

    return 0;
}

int usb_msc_write_sectors(struct xhci_controller *hc, struct xhci_device *dev,
                          uint32_t lba, uint32_t count, const void *buf)
{
    struct usb_msc_info *info;
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t max_blocks;

    if (dev->slot_id >= XHCI_MAX_DEVICES)
        return -1;
    info = &msc_info[dev->slot_id];
    if (!info->valid || info->sector_size == 0)
        return -1;

    if (count == 0)
        return 0;
    /* Reject out-of-range writes before the 32-bit LBA chunk loop -- a wrap
     * here would overwrite the start of the medium. */
    if (lba >= info->sector_count || count > info->sector_count - lba)
        return -1;

    /* Same 64 KiB single-TRB + 16-bit WRITE(10) block-count limits as the
     * read path; chunk so neither overflows. */
    max_blocks = MSC_MAX_XFER_BYTES / info->sector_size;
    if (max_blocks == 0)
        max_blocks = 1;
    if (max_blocks > 0xFFFF)
        max_blocks = 0xFFFF;

    while (count > 0) {
        uint32_t chunk = count < max_blocks ? count : max_blocks;
        uint32_t data_len = chunk * info->sector_size;
        uint8_t cdb[10];
        int rc;

        msc_zero(cdb, 10);
        cdb[0] = SCSI_WRITE_10;
        /* LBA (big-endian) */
        cdb[2] = (uint8_t)(lba >> 24);
        cdb[3] = (uint8_t)(lba >> 16);
        cdb[4] = (uint8_t)(lba >> 8);
        cdb[5] = (uint8_t)(lba);
        /* Transfer length in sectors (big-endian) */
        cdb[7] = (uint8_t)(chunk >> 8);
        cdb[8] = (uint8_t)(chunk);

        rc = msc_bot_command(hc, dev, cdb, 10, (void *)in, data_len, 0);
        if (rc != USB_CSW_STATUS_PASS) {
            klog(LOG_ERROR, "usb-msc", "WRITE(10) failed: LBA=%u count=%u status=%d",
                 (uint64_t)lba, (uint64_t)chunk, (uint64_t)rc);
            return -1;
        }

        lba += chunk;
        in += data_len;
        count -= chunk;
    }

    return 0;
}

const struct usb_msc_info *usb_msc_get_info(struct xhci_device *dev)
{
    if (dev->slot_id >= XHCI_MAX_DEVICES)
        return NULL;
    struct usb_msc_info *info = &msc_info[dev->slot_id];
    return info->valid ? info : NULL;
}
