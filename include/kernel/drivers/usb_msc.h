/* ============================================================================
 * usb_msc.h -- USB Mass Storage Class (Bulk-Only Transport) driver
 *
 * Implements SCSI-over-USB via CBW/CSW framing.
 * Reference: USB Mass Storage Class Bulk-Only Transport Revision 1.0
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Forward declarations */
struct xhci_controller;
struct xhci_device;

/* ---- CBW / CSW structures (USB BOT spec §5) ---- */

#define USB_MSC_CBW_SIGNATURE  0x43425355  /* "USBC" */
#define USB_MSC_CSW_SIGNATURE  0x53425355  /* "USBS" */
#define USB_MSC_CBW_SIZE       31
#define USB_MSC_CSW_SIZE       13

/* CBW -- Command Block Wrapper (31 bytes) */
struct usb_cbw {
    uint32_t dCBWSignature;          /* USB_MSC_CBW_SIGNATURE */
    uint32_t dCBWTag;                /* Tag to match with CSW */
    uint32_t dCBWDataTransferLength; /* Bytes to transfer in data phase */
    uint8_t  bmCBWFlags;             /* Bit 7: 0=OUT, 1=IN */
    uint8_t  bCBWLUN;                /* Logical Unit Number (usually 0) */
    uint8_t  bCBWCBLength;           /* Length of CBWCB (1-16) */
    uint8_t  CBWCB[16];             /* Command Block (SCSI CDB) */
} __attribute__((packed));

/* CSW -- Command Status Wrapper (13 bytes) */
struct usb_csw {
    uint32_t dCSWSignature;          /* USB_MSC_CSW_SIGNATURE */
    uint32_t dCSWTag;                /* Must match CBW tag */
    uint32_t dCSWDataResidue;        /* Difference between expected and actual */
    uint8_t  bCSWStatus;             /* 0=pass, 1=fail, 2=phase error */
} __attribute__((packed));

#define USB_CSW_STATUS_PASS   0
#define USB_CSW_STATUS_FAIL   1
#define USB_CSW_STATUS_PHASE  2

/* CBW flags */
#define USB_CBW_FLAG_IN   0x80
#define USB_CBW_FLAG_OUT  0x00

/* ---- SCSI command opcodes ---- */

#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_REQUEST_SENSE    0x03
#define SCSI_INQUIRY          0x12
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10          0x28
#define SCSI_WRITE_10         0x2A

/* SCSI sense keys (SPC-4 4.5.6) -- shared single source of truth. */
#include "kernel/drivers/scsi.h"

#define SCSI_SENSE_LEN          18   /* fixed-format REQUEST SENSE allocation */

/* Error class returned by msc_sense_classify -- drives the retry policy in the
 * MSC BOT transient-error retry layer. */
typedef enum {
    MSC_ERR_OK = 0,        /* no error / recovered -- proceed */
    MSC_ERR_RETRY_NOW,     /* UNIT ATTENTION: device reset itself, retry now */
    MSC_ERR_WAIT_RETRY,    /* NOT READY: spinning up, wait + retry */
    MSC_ERR_UNRECOVERABLE  /* MEDIUM/HARDWARE/etc: no retry will help */
} msc_err_class_t;

/* ---- MSC device info (populated after inquiry + read capacity) ---- */

struct usb_msc_info {
    uint8_t  valid;              /* 1 if info populated */
    uint8_t  device_type;        /* SCSI peripheral device type */
    char     vendor[9];          /* 8-char vendor + null */
    char     product[17];        /* 16-char product + null */
    uint32_t sector_count;       /* Total sectors (LBA count) */
    uint32_t sector_size;        /* Bytes per sector (usually 512) */
    uint64_t capacity_bytes;     /* Total capacity */
    uint8_t  current_lun;        /* LUN written into every CBW for this device */
    uint8_t  max_lun;            /* Highest LUN index (GET_MAX_LUN; 0 = single) */
};

/* ---- API ---- */

/* Initialize the MSC driver for a specific USB device.
 * Sends INQUIRY + READ CAPACITY, populates msc_info.
 * Returns 0 on success. */
int usb_msc_init(struct xhci_controller *hc, struct xhci_device *dev);

/* Read sectors from USB mass storage device.
 * Returns 0 on success, -1 on failure. */
int usb_msc_read_sectors(struct xhci_controller *hc, struct xhci_device *dev,
                         uint32_t lba, uint32_t count, void *buf);

/* Write sectors to USB mass storage device.
 * Returns 0 on success, -1 on failure. */
int usb_msc_write_sectors(struct xhci_controller *hc, struct xhci_device *dev,
                          uint32_t lba, uint32_t count, const void *buf);

/* Get MSC device info (valid after usb_msc_init). */
const struct usb_msc_info *usb_msc_get_info(struct xhci_device *dev);

/* Classify a fixed-format SCSI sense buffer (>= 18 bytes) into a retry policy
 * class. Pure: reads only the sense key (byte 2 bits 3:0). Exposed for tests. */
msc_err_class_t msc_sense_classify(const uint8_t *sense);

/* Human-readable name for a SCSI sense key (0x0-0xF). Pure; for diagnostics. */
const char *msc_sense_key_name(uint8_t key);
