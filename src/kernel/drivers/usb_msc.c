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
    cbw.bCBWLUN = 0;
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

    /* TEST UNIT READY -- some devices need a few attempts */
    for (retries = 0; retries < 3; retries++) {
        rc = msc_test_unit_ready(hc, dev);
        if (rc == USB_CSW_STATUS_PASS)
            break;
        msc_delay_us(100000);  /* 100ms between retries */
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
    uint8_t cdb[10];
    uint32_t data_len;
    struct usb_msc_info *info;
    int rc;

    if (dev->slot_id >= XHCI_MAX_DEVICES)
        return -1;
    info = &msc_info[dev->slot_id];
    if (!info->valid)
        return -1;

    data_len = count * info->sector_size;

    msc_zero(cdb, 10);
    cdb[0] = SCSI_READ_10;
    /* LBA (big-endian) */
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)(lba);
    /* Transfer length in sectors (big-endian) */
    cdb[7] = (uint8_t)(count >> 8);
    cdb[8] = (uint8_t)(count);

    rc = msc_bot_command(hc, dev, cdb, 10, buf, data_len, 1);
    if (rc != USB_CSW_STATUS_PASS) {
        klog(LOG_ERROR, "usb-msc", "READ(10) failed: LBA=%u count=%u status=%d",
             (uint64_t)lba, (uint64_t)count, (uint64_t)rc);
        return -1;
    }

    return 0;
}

int usb_msc_write_sectors(struct xhci_controller *hc, struct xhci_device *dev,
                          uint32_t lba, uint32_t count, const void *buf)
{
    uint8_t cdb[10];
    uint32_t data_len;
    struct usb_msc_info *info;
    int rc;

    if (dev->slot_id >= XHCI_MAX_DEVICES)
        return -1;
    info = &msc_info[dev->slot_id];
    if (!info->valid)
        return -1;

    data_len = count * info->sector_size;

    msc_zero(cdb, 10);
    cdb[0] = SCSI_WRITE_10;
    /* LBA (big-endian) */
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)(lba);
    /* Transfer length in sectors (big-endian) */
    cdb[7] = (uint8_t)(count >> 8);
    cdb[8] = (uint8_t)(count);

    rc = msc_bot_command(hc, dev, cdb, 10, (void *)buf, data_len, 0);
    if (rc != USB_CSW_STATUS_PASS) {
        klog(LOG_ERROR, "usb-msc", "WRITE(10) failed: LBA=%u count=%u status=%d",
             (uint64_t)lba, (uint64_t)count, (uint64_t)rc);
        return -1;
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
