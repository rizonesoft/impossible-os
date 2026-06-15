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
#include "kernel/timer.h"

/* ---- Static state ---- */

/* Per-command transfer cap: one xHCI Normal TRB carries at most 64 KiB
 * (17-bit TRB Transfer Length). READ(10)/WRITE(10) further cap the block
 * count at 16 bits; both bounds are enforced by chunking in the sector ops. */
#define MSC_MAX_XFER_BYTES   65536u

/* TEST UNIT READY readiness poll (event-driven, replaces a fixed post-init
 * sleep). A ready drive returns on the first attempt with zero added latency;
 * a spinning-up drive is polled out under a real wall-clock budget. The budget
 * is a true elapsed-time bound (each TUR/REQUEST SENSE cycle can itself stall
 * up to the 500 ms xHCI transfer timeout, so an attempt count alone is NOT a
 * time bound); MSC_TUR_MAX_ATTEMPTS is only a backstop for the pre-timer path
 * where uptime_ns() reads 0. */
#define MSC_TUR_READY_BUDGET_MS  2000u   /* wall-clock readiness budget */
#define MSC_TUR_POLL_US          50000u  /* 50 ms between readiness retries */
#define MSC_TUR_MAX_ATTEMPTS     40u     /* backstop when no timer is available */

/* Per-device MSC state. Keyed by the GLOBAL device index (xhci_device_index),
 * NOT the controller-local slot_id: xHCI assigns slot IDs per controller, so two
 * controllers can each enumerate a device at slot 1. Keying by slot_id would
 * alias both into msc_info[1] and corrupt one device's geometry/LUN/retry state.
 * The global index is unique across all controllers. */
static struct usb_msc_info msc_info[XHCI_MAX_DEVICES];
static uint32_t cbw_tag = 1;

/* ---- Helpers ---- */

/* Resolve a device to its msc_info slot via the global device index. Returns
 * NULL if dev is not a tracked device. */
static struct usb_msc_info *msc_state(struct xhci_device *dev)
{
    int idx = xhci_device_index(dev);
    if (idx < 0 || idx >= XHCI_MAX_DEVICES)
        return (struct usb_msc_info *)0;
    return &msc_info[idx];
}

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

/* ---- BOT transport-level recovery (stall/halt) ---- */

/* Max endpoint-recovery retries for a single bulk transfer. */
#define MSC_STALL_RETRIES   2

/* Per-direction bulk pipe accessors. */
static struct xhci_ring *msc_bulk_ring(struct xhci_device *dev, int dir_in)
{
    return dir_in ? &dev->bulk_in_ring : &dev->bulk_out_ring;
}
static uint32_t msc_bulk_dci(struct xhci_device *dev, int dir_in)
{
    return dir_in ? XHCI_DCI(dev->bulk_in_ep, 1) : XHCI_DCI(dev->bulk_out_ep, 0);
}
static uint8_t msc_bulk_addr(struct xhci_device *dev, int dir_in)
{
    return dir_in ? dev->bulk_in_addr : dev->bulk_out_addr;
}

/* True if a completion code is a recoverable endpoint halt (all leave the
 * endpoint Halted and clear with the stall-recovery sequence). Exposed for
 * tests. */
int msc_cc_is_halt(uint8_t cc)
{
    return cc == XHCI_TRB_CC_STALL || cc == XHCI_TRB_CC_BABBLE ||
           cc == XHCI_TRB_CC_USB_TXN || cc == XHCI_TRB_CC_DATA_BUFFER;
}

/* Re-sync one bulk pipe after a BOT reset. xhci_recover_endpoint reads the
 * endpoint's actual EP State and does exactly the steps it needs: device-side
 * CLEAR_FEATURE always, plus host-side Reset Endpoint (only if Halted) and Set
 * TR Dequeue (Halted or Stopped) so a partially-recovered Stopped pipe is
 * re-armed rather than silently left un-usable. Returns 0/-1. */
static int msc_reset_bulk_pipe(struct xhci_controller *hc, struct xhci_device *dev,
                               int dir_in)
{
    return xhci_recover_endpoint(hc, dev, msc_bulk_dci(dev, dir_in),
                                 msc_bulk_addr(dev, dir_in),
                                 msc_bulk_ring(dev, dir_in));
}

/* BOT mass-storage reset (BOT 1.0 section 5.3.4): class Reset request to the
 * MSC interface, then re-sync BOTH bulk pipes (device CLEAR_FEATURE plus
 * host-side Reset Endpoint + Set TR Dequeue when the endpoint context is still
 * Halted). Fails if the class reset or either pipe re-sync fails -- a partial
 * reset that claimed success would run later CBW/CSW against a halted pipe.
 * Returns 0 on success, -1 on failure. */
static int msc_bot_reset(struct xhci_controller *hc, struct xhci_device *dev)
{
    uint8_t setup[8];
    int rc = 0;

    setup[0] = 0x21;  /* bmRequestType: Host-to-Device, Class, Interface */
    setup[1] = 0xFF;  /* bRequest = Bulk-Only Mass Storage Reset */
    setup[2] = 0x00;  /* wValue = 0 */
    setup[3] = 0x00;
    setup[4] = dev->msc_iface;  /* wIndex = interface number */
    setup[5] = 0x00;
    setup[6] = 0x00;  /* wLength = 0 */
    setup[7] = 0x00;
    if (xhci_send_control(hc, dev, setup, NULL, 0, 0) != 0) {
        klog(LOG_ERROR, "usb-msc", "BOT mass-storage reset failed (slot %u)",
             (uint64_t)dev->slot_id);
        return -1;
    }
    if (msc_reset_bulk_pipe(hc, dev, 1) != 0)   /* bulk-IN */
        rc = -1;
    if (msc_reset_bulk_pipe(hc, dev, 0) != 0)   /* bulk-OUT */
        rc = -1;
    if (rc != 0)
        klog(LOG_ERROR, "usb-msc", "BOT reset: bulk pipe re-sync failed (slot %u)",
             (uint64_t)dev->slot_id);
    else
        klog(LOG_WARN, "usb-msc", "BOT mass-storage reset (slot %u)",
             (uint64_t)dev->slot_id);
    return rc;
}

/* Bulk transfer with transport-level stall recovery. On a recoverable halt,
 * recover the endpoint and retry the SAME transfer up to MSC_STALL_RETRIES
 * times (safe for the idempotent CBW and CSW phases). Returns 0 on success
 * (SUCCESS/SHORT_PKT), -1 if unrecoverable or a non-halt transport error. */
static int msc_bot_xfer(struct xhci_controller *hc, struct xhci_device *dev,
                        int dir_in, void *buf, uint32_t len)
{
    struct xhci_ring *ring = msc_bulk_ring(dev, dir_in);
    uint32_t dci = msc_bulk_dci(dev, dir_in);
    uint8_t ep_addr = msc_bulk_addr(dev, dir_in);
    int attempt;

    for (attempt = 0; attempt <= MSC_STALL_RETRIES; attempt++) {
        uint8_t cc = xhci_bulk_transfer_cc(hc, dev, ring, buf, len, dir_in, NULL);
        if (cc == XHCI_TRB_CC_SUCCESS || cc == XHCI_TRB_CC_SHORT_PKT)
            return 0;
        if (!msc_cc_is_halt(cc))
            return -1;        /* non-halt transport error -- not recoverable here */
        if (xhci_recover_endpoint(hc, dev, dci, ep_addr, ring) != 0)
            return -1;        /* recovery itself failed */
        /* recovered -- loop to retry */
    }
    return -1;                /* still halted after retries */
}

/* ---- BOT transport ---- */

/* Execute a BOT command: send CBW, optional data phase, receive CSW.
 * Returns CSW status (0=pass, 1=fail, 2=phase error) or -1 on transport error.
 * On a PASS CSW, `*residue_out` (when non-NULL) receives the CSW data residue
 * (bytes NOT transferred); a residue greater than the requested length is a
 * malformed CSW and is rejected as a transport failure with a BOT reset.
 * `*data_xfer_out` (when non-NULL) receives the HOST-observed bytes moved in the
 * data phase (independent of the device's CSW residue), so an exact-length
 * caller can fail-safe on a host short the CSW residue might not report. */
static int msc_bot_command(struct xhci_controller *hc, struct xhci_device *dev,
                           const uint8_t *cdb, uint8_t cdb_len,
                           void *data, uint32_t data_len, int dir_in,
                           uint32_t *residue_out, uint32_t *data_xfer_out)
{
    struct usb_cbw cbw;
    struct usb_csw csw;
    uint32_t tag = cbw_tag++;
    int data_short = 0;       /* set if the data phase halted mid-transfer */
    uint32_t data_xfer = 0;   /* host-observed bytes moved in the data phase */

    if (residue_out)
        *residue_out = 0;
    if (data_xfer_out)
        *data_xfer_out = 0;

    /* Build CBW */
    msc_zero(&cbw, sizeof(cbw));
    cbw.dCBWSignature = USB_MSC_CBW_SIGNATURE;
    cbw.dCBWTag = tag;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags = dir_in ? USB_CBW_FLAG_IN : USB_CBW_FLAG_OUT;
    /* Address the device's currently-selected LUN (0 unless boot-LUN selection
     * picked another for composite media like card readers). */
    {
        struct usb_msc_info *st = msc_state(dev);
        cbw.bCBWLUN = st ? st->current_lun : 0;
    }
    cbw.bCBWCBLength = cdb_len;
    msc_copy(cbw.CBWCB, cdb, cdb_len);

    /* Send CBW via Bulk-OUT (transport-level stall recovery + retry). */
    if (msc_bot_xfer(hc, dev, 0, &cbw, USB_MSC_CBW_SIZE) != 0) {
        klog(LOG_ERROR, "usb-msc", "CBW send failed");
        return -1;
    }

    /* Data phase (optional). BOT 1.0 section 6.7: a STALL during the data phase
     * is recovered by clearing the endpoint halt and PROCEEDING to the CSW (the
     * device reports the short/failed transfer in the CSW residue) -- the data
     * is NOT re-sent, which would desync a WRITE. A non-halt transport error is
     * unrecoverable here. */
    if (data && data_len > 0) {
        struct xhci_ring *ring = msc_bulk_ring(dev, dir_in);
        uint8_t cc = xhci_bulk_transfer_cc(hc, dev, ring, data, data_len, dir_in,
                                           &data_xfer);
        if (cc != XHCI_TRB_CC_SUCCESS && cc != XHCI_TRB_CC_SHORT_PKT) {
            if (!msc_cc_is_halt(cc)) {
                klog(LOG_ERROR, "usb-msc", "Data phase failed (%s, %u bytes, cc=%u)",
                     dir_in ? "IN" : "OUT", (uint64_t)data_len, (uint64_t)cc);
                return -1;
            }
            /* Recover the halted data endpoint, then fall through to the CSW. */
            if (xhci_recover_endpoint(hc, dev, msc_bulk_dci(dev, dir_in),
                                      msc_bulk_addr(dev, dir_in), ring) != 0) {
                klog(LOG_ERROR, "usb-msc", "Data-phase stall recovery failed");
                return -1;
            }
            /* The data phase was demonstrably incomplete (halted mid-transfer).
             * Read the CSW to re-sync the pipe, but never report this command as
             * a complete transfer -- a caller (e.g. READ(10)) must not advance
             * the LBA on partial data. Per-command residue acceptance (a short
             * INQUIRY is legal) is the comprehensive owner's job. */
            data_short = 1;
        }
    }

    /* Receive CSW via Bulk-IN (transport-level stall recovery + retry). */
    msc_zero(&csw, sizeof(csw));
    if (msc_bot_xfer(hc, dev, 1, &csw, USB_MSC_CSW_SIZE) != 0) {
        klog(LOG_ERROR, "usb-msc", "CSW receive failed -- BOT reset");
        msc_bot_reset(hc, dev);
        return -1;
    }

    /* Validate CSW. A bad signature or tag mismatch means the BOT pipe is
     * desynced (the bytes are not a CSW for this command) -- a mass-storage
     * reset re-syncs both pipes; returning -1 without it would leave every
     * subsequent command misframed. */
    if (csw.dCSWSignature != USB_MSC_CSW_SIGNATURE) {
        klog(LOG_ERROR, "usb-msc", "CSW bad signature: 0x%08x -- BOT reset",
             (uint64_t)csw.dCSWSignature);
        msc_bot_reset(hc, dev);
        return -1;
    }
    if (csw.dCSWTag != tag) {
        klog(LOG_ERROR, "usb-msc", "CSW tag mismatch: got %u, expected %u -- BOT reset",
             (uint64_t)csw.dCSWTag, (uint64_t)tag);
        msc_bot_reset(hc, dev);
        return -1;
    }
    /* A residue larger than the requested length is a malformed CSW for ANY
     * status -- the device claims it skipped more than was asked, so the framing
     * is untrustworthy. Reject (reset + fail) before any status-specific handling
     * or REQUEST SENSE, so recovery never runs against a CSW the code distrusts. */
    if (csw.dCSWDataResidue > data_len) {
        klog(LOG_ERROR, "usb-msc", "CSW residue %u > requested %u -- BOT reset",
             (uint64_t)csw.dCSWDataResidue, (uint64_t)data_len);
        msc_bot_reset(hc, dev);
        return -1;
    }

    /* A CSW phase error (bCSWStatus == 2) means the device and host disagree on
     * the transfer framing -- BOT 1.0 section 6.7 mandates a mass-storage reset
     * before any further command. If the reset itself fails the pipe is NOT
     * re-synced, so report a transport failure (-1) rather than the retryable
     * phase status -- a caller must never re-issue a CBW into an unreset pipe. */
    if (csw.bCSWStatus == USB_CSW_STATUS_PHASE) {
        klog(LOG_WARN, "usb-msc", "CSW phase error -- BOT reset (slot %u)",
             (uint64_t)dev->slot_id);
        if (msc_bot_reset(hc, dev) != 0)
            return -1;
    }

    /* A recovered data-phase stall means the data phase did not complete; even
     * if the device returns CSW PASS, the transfer is short. Fail the command so
     * no caller treats it as a full transfer. */
    if (data_short && csw.bCSWStatus == USB_CSW_STATUS_PASS) {
        klog(LOG_WARN, "usb-msc", "data phase short after stall recovery -- failing command");
        return -1;
    }

    /* Report the data residue on a PASS CSW so the SCSI layer can fail-safe on a
     * short exact-length transfer (residue <= data_len already validated above). */
    if (csw.bCSWStatus == USB_CSW_STATUS_PASS && residue_out)
        *residue_out = csw.dCSWDataResidue;
    /* Report the host-observed data-phase byte count (authoritative xHCI-level
     * length, independent of the device's CSW residue). */
    if (data_xfer_out)
        *data_xfer_out = data_xfer;

    return csw.bCSWStatus;
}

/* ---- SCSI commands ---- */

static int msc_test_unit_ready(struct xhci_controller *hc, struct xhci_device *dev)
{
    uint8_t cdb[6];
    msc_zero(cdb, 6);
    cdb[0] = SCSI_TEST_UNIT_READY;
    return msc_bot_command(hc, dev, cdb, 6, NULL, 0, 0, NULL, NULL);
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
                                         struct xhci_device *dev, uint8_t *sense,
                                         int *rs_rc)
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

    {
        uint32_t residue = 0;
        rc = msc_bot_command(hc, dev, cdb, 6, sense, SCSI_SENSE_LEN, 1, &residue, NULL);
        /* Report the BOT command status so callers can tell a transport-level
         * REQUEST SENSE failure (pipe desync during recovery) apart from a
         * successful sense read that merely classifies as unrecoverable. */
        if (rs_rc)
            *rs_rc = rc;
        if (rc != USB_CSW_STATUS_PASS) {
            klog(LOG_WARN, "usb-msc", "REQUEST SENSE failed (status=%d)", (uint64_t)rc);
            return MSC_ERR_UNRECOVERABLE;
        }
        /* The classification reads the response code (0), sense key (2), ASC
         * (12), and ASCQ (13). If the device returned fewer than 14 bytes the
         * key/ASC/ASCQ come from the zeroed tail and would misclassify as
         * NO SENSE/OK -- reject a short sense (CSW residue is the actual length,
         * available without the per-transfer byte count). */
        if (SCSI_SENSE_LEN - residue < SCSI_SENSE_MIN) {
            klog(LOG_WARN, "usb-msc", "REQUEST SENSE short (%u of %u bytes)",
                 (uint64_t)(SCSI_SENSE_LEN - residue), (uint64_t)SCSI_SENSE_LEN);
            return MSC_ERR_UNRECOVERABLE;
        }
    }
    klog(LOG_INFO, "usb-msc", "Sense: key=%u ASC=0x%02x ASCQ=0x%02x (%s)",
         (uint64_t)(sense[2] & 0x0F), (uint64_t)sense[12], (uint64_t)sense[13],
         msc_sense_key_name(sense[2]));
    return msc_sense_classify(sense);
}

/* Pure readiness-poll decision (exposed for tests). See header for contract. */
msc_tur_action_t msc_tur_decide(int rc, int rs_rc, msc_err_class_t cls)
{
    if (rc == USB_CSW_STATUS_PASS)
        return MSC_TUR_READY;
    /* A non-FAIL TUR result (phase error 2, transport < 0, out-of-range) means
     * the BOT pipe is desynced -- another CBW would compound it, so abort and
     * let the transport-recovery layer reset the pipe. */
    if (rc != USB_CSW_STATUS_FAIL)
        return MSC_TUR_ABORT;
    /* TUR returned CHECK CONDITION -> REQUEST SENSE was issued. Only a REQUEST
     * SENSE phase error / transport failure (not a plain CSW FAIL) means the
     * pipe desynced DURING recovery: a CSW FAIL is a framed command-level
     * failure delivered on a still-synced pipe, so it is a GIVEUP, not an
     * ABORT. */
    if (rs_rc != USB_CSW_STATUS_PASS && rs_rc != USB_CSW_STATUS_FAIL)
        return MSC_TUR_ABORT;
    /* REQUEST SENSE itself returned CSW FAIL (anomalous per SPC-4, but the pipe
     * is synced), or it succeeded with a hard sense (MEDIUM/HARDWARE/malformed):
     * either way stop polling and let init warn and continue (INQUIRY/READ
     * CAPACITY surface the real error) rather than abort. */
    if (rs_rc == USB_CSW_STATUS_FAIL || cls == MSC_ERR_UNRECOVERABLE)
        return MSC_TUR_GIVEUP;
    /* NOT READY (spin-up), UNIT ATTENTION (post-plug reset), or NO SENSE: a
     * transient condition that a short wait + retry can clear. */
    return MSC_TUR_WAIT;
}

/* Poll outcome -- distinguishes a safe-to-continue readiness failure (device
 * may answer later, or reported a hard error on a healthy pipe) from a
 * transport desync that must NOT receive another CBW until BOT mass-storage
 * reset / endpoint-stall recovery lands. */
typedef enum {
    MSC_POLL_READY = 0,    /* device reported ready */
    MSC_POLL_NOT_READY,    /* not ready (budget spent or hard error) -- continue */
    MSC_POLL_ABORT         /* BOT pipe desync -- abort MSC init */
} msc_poll_result_t;

/* Poll TEST UNIT READY until the device is ready, decoding each failure via
 * REQUEST SENSE so the sense class drives the wait. Bounded by a real elapsed
 * wall-clock budget (uptime_ns), checked BEFORE each command so no CBW is
 * issued past the budget; an attempt-count backstop bounds the path where no
 * timer is up yet (uptime_ns reads 0). `*polls_out` receives the number of TUR
 * commands actually issued (for the serial poll-count diagnostic). Returns one
 * of msc_poll_result_t. */
static msc_poll_result_t msc_poll_unit_ready(struct xhci_controller *hc,
                                             struct xhci_device *dev,
                                             uint32_t *polls_out)
{
    uint64_t start = uptime_ns();
    uint64_t budget_ns = (uint64_t)MSC_TUR_READY_BUDGET_MS * 1000000ull;
    uint32_t attempt;
    uint32_t polls = 0;

    for (attempt = 0; attempt < MSC_TUR_MAX_ATTEMPTS; attempt++) {
        int rc;
        int rs_rc = USB_CSW_STATUS_PASS;  /* REQUEST SENSE not issued unless FAIL */
        msc_err_class_t cls = MSC_ERR_OK;

        /* Enforce the wall-clock budget BEFORE issuing another command.
         * Elapsed arithmetic (now - start) is wrap-safe for an unsigned
         * monotonic counter; deadline addition is not. start == 0 means no
         * timer is up yet -- fall through to the attempt-count backstop. */
        if (start != 0 && (uptime_ns() - start) >= budget_ns)
            break;

        rc = msc_test_unit_ready(hc, dev);
        polls++;
        if (rc == USB_CSW_STATUS_FAIL) {
            uint8_t sense[SCSI_SENSE_LEN];
            cls = msc_request_sense(hc, dev, sense, &rs_rc);
        }
        switch (msc_tur_decide(rc, rs_rc, cls)) {
        case MSC_TUR_READY:
            if (polls_out) *polls_out = polls;
            return MSC_POLL_READY;
        case MSC_TUR_ABORT:
            if (polls_out) *polls_out = polls;
            return MSC_POLL_ABORT;
        case MSC_TUR_GIVEUP:
            if (polls_out) *polls_out = polls;
            return MSC_POLL_NOT_READY;  /* hard error, pipe healthy -- continue */
        case MSC_TUR_WAIT:
        default:
            break;
        }
        msc_delay_us(MSC_TUR_POLL_US);
    }
    if (polls_out) *polls_out = polls;
    return MSC_POLL_NOT_READY;
}

/* ---- SCSI whole-command retry (transient errors) ---- */

#define MSC_SCSI_RETRIES   3        /* whole-command attempts before failing */
#define MSC_SCSI_WAIT_US   100000   /* 100 ms NOT-READY (spin-up) retry wait */

/* Pure: is a CSW residue a disallowed short transfer? See header. */
int msc_residue_short(int exact_len, uint32_t residue)
{
    return exact_len && residue != 0;
}

/* Pure: is the host-observed transfer short for an exact-length command? */
int msc_host_short(int exact_len, uint32_t requested, uint32_t transferred)
{
    return exact_len && transferred < requested;
}

/* Pure whole-command retry decision (exposed for tests). See header contract. */
msc_cmd_action_t msc_scsi_retry_decide(int rc, int rs_rc, msc_err_class_t cls,
                                       int exact_short)
{
    if (rc == USB_CSW_STATUS_PASS)
        return exact_short ? MSC_CMD_FAIL : MSC_CMD_DONE;
    if (rc == USB_CSW_STATUS_PHASE)
        return MSC_CMD_RETRY;        /* msc_bot_command already BOT-reset */
    if (rc != USB_CSW_STATUS_FAIL)
        return MSC_CMD_RESET_RETRY;  /* transport (<0): pipe state unknown */
    /* CSW FAIL: REQUEST SENSE was issued to read the sense. If the sense probe
     * itself hit a phase/transport failure (rs_rc neither PASS nor a framed
     * FAIL) the pipe may be desynced -- reset before retry rather than trust the
     * unreadable sense class. */
    if (rs_rc != USB_CSW_STATUS_PASS && rs_rc != USB_CSW_STATUS_FAIL)
        return MSC_CMD_RESET_RETRY;
    /* Sense read on a synced pipe -- the class drives the retry. */
    if (cls == MSC_ERR_UNRECOVERABLE)
        return MSC_CMD_FAIL;         /* MEDIUM/HARDWARE/short sense -- no retry */
    if (cls == MSC_ERR_WAIT_RETRY)
        return MSC_CMD_RETRY_WAIT;   /* NOT READY -- spin-up wait */
    /* UNIT ATTENTION or NO SENSE/recovered: the command still FAILED, so retry
     * (bounded) -- never report success off a benign sense. */
    return MSC_CMD_RETRY;
}

/* Execute a SCSI command with whole-command retry on transient errors. Wraps
 * msc_bot_command (which already does transport stall recovery + BOT reset on a
 * desynced CSW): on a transient CSW FAIL it decodes the sense and retries per
 * the class; on a transport failure it BOT-resets before re-issuing so a fresh
 * CBW never goes into an unknown pipe state. `exact_len` = 1 for fixed-length
 * commands (READ/WRITE/READ CAPACITY) where any CSW residue is a short transfer
 * that must fail. Returns 0 on success, -1 after MSC_SCSI_RETRIES attempts or an
 * unrecoverable error. */
static int msc_scsi_command(struct xhci_controller *hc, struct xhci_device *dev,
                            const uint8_t *cdb, uint8_t cdb_len,
                            void *data, uint32_t data_len, int dir_in,
                            int exact_len, uint32_t *actual_out)
{
    int attempt;

    if (actual_out)
        *actual_out = 0;

    for (attempt = 0; attempt < MSC_SCSI_RETRIES; attempt++) {
        uint32_t residue = 0;
        uint32_t host_xfer = 0;
        int rc = msc_bot_command(hc, dev, cdb, cdb_len, data, data_len, dir_in,
                                 &residue, &host_xfer);
        /* Fail an exact-length command on EITHER a CSW residue short OR a
         * host-observed short: the device's CSW residue can misreport a short
         * data phase, so the host transfer count is the authoritative guard. */
        int exact_short = msc_residue_short(exact_len, residue) ||
                          msc_host_short(exact_len, data_len, host_xfer);
        int rs_rc = USB_CSW_STATUS_PASS;  /* REQUEST SENSE not issued unless FAIL */
        msc_err_class_t cls = MSC_ERR_OK;

        if (rc == USB_CSW_STATUS_FAIL) {
            uint8_t sense[SCSI_SENSE_LEN];
            cls = msc_request_sense(hc, dev, sense, &rs_rc);
        }

        switch (msc_scsi_retry_decide(rc, rs_rc, cls, exact_short)) {
        case MSC_CMD_DONE:
            /* Report the HOST-observed bytes transferred so an allocation-length
             * caller (e.g. INQUIRY min-length) enforces its minimum on the
             * authoritative xHCI count -- never on the device's CSW residue,
             * which a flaky device can misreport (residue 0 on a short reply). */
            if (actual_out)
                *actual_out = host_xfer;
            return 0;
        case MSC_CMD_FAIL:
            return -1;
        case MSC_CMD_RETRY:
            break;                   /* retry now */
        case MSC_CMD_RETRY_WAIT:
            msc_delay_us(MSC_SCSI_WAIT_US);
            break;
        case MSC_CMD_RESET_RETRY:
        default:
            if (msc_bot_reset(hc, dev) != 0)
                return -1;           /* cannot re-sync the pipe -- give up */
            break;
        }
        klog(LOG_DEBUG, "usb-msc", "Retry %d/%d: cdb 0x%02x (rc=%d, sense=%u)",
             attempt + 1, MSC_SCSI_RETRIES, (uint64_t)cdb[0], (uint64_t)rc,
             (uint64_t)cls);
    }
    klog(LOG_WARN, "usb-msc", "SCSI cdb 0x%02x failed after %d retries",
         (uint64_t)cdb[0], MSC_SCSI_RETRIES);
    return -1;
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
    /* INQUIRY is allocation-length (exact_len=0): a device may legitimately
     * return fewer than 36 bytes. Require at least the standard 5-byte header so
     * the device_type (byte 0) is real; the vendor/product fields (bytes 8-31)
     * stay zero-filled (empty strings) on a short reply, which is cosmetic. */
    {
        uint32_t actual = 0;
        rc = msc_scsi_command(hc, dev, cdb, 6, buf, 36, 1, 0, &actual);
        if (rc != 0) {
            klog(LOG_ERROR, "usb-msc", "INQUIRY failed");
            return -1;
        }
        if (actual < SCSI_INQUIRY_MIN) {
            klog(LOG_ERROR, "usb-msc", "INQUIRY too short (%u bytes)", (uint64_t)actual);
            return -1;
        }
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
    /* exact_len=1: READ CAPACITY(10) returns exactly 8 bytes. */
    rc = msc_scsi_command(hc, dev, cdb, 10, buf, 8, 1, 1, NULL);
    if (rc != 0) {
        klog(LOG_ERROR, "usb-msc", "READ CAPACITY failed");
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

    if (!dev->is_msc) {
        klog(LOG_DEBUG, "usb-msc", "Slot %u is not MSC", (uint64_t)dev->slot_id);
        return -1;
    }

    info = msc_state(dev);
    if (!info)
        return -1;
    msc_zero(info, sizeof(*info));

    /* Wait for the device to report ready (replaces a fixed post-init sleep).
     * A ready drive returns immediately; a spinning-up drive is polled out
     * under a wall-clock budget. A transport-level desync (phase error / bad
     * CSW) aborts MSC init -- sending INQUIRY into a desynced BOT pipe before
     * mass-storage reset exists only compounds the failure. */
    uint32_t tur_polls = 0;
    switch (msc_poll_unit_ready(hc, dev, &tur_polls)) {
    case MSC_POLL_READY:
        if (tur_polls > 1)
            klog(LOG_INFO, "usb-msc", "USB drive ready after %u TUR poll(s)",
                 (uint64_t)tur_polls);
        break;
    case MSC_POLL_NOT_READY:
        klog(LOG_WARN, "usb-msc",
             "USB drive not ready within %u ms (%u TUR poll(s)), continuing",
             (uint64_t)MSC_TUR_READY_BUDGET_MS, (uint64_t)tur_polls);
        break;
    case MSC_POLL_ABORT:
    default:
        klog(LOG_ERROR, "usb-msc",
             "BOT transport desync during readiness poll (%u TUR poll(s)) -- "
             "aborting MSC init (awaiting transport recovery)",
             (uint64_t)tur_polls);
        return -1;
    }

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

    info = msc_state(dev);
    if (!info || !info->valid || info->sector_size == 0)
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

        /* exact_len=1: a short data phase on a boot READ must FAIL SAFE -- never
         * advance the LBA / return partial sectors as if the read completed. */
        rc = msc_scsi_command(hc, dev, cdb, 10, out, data_len, 1, 1, NULL);
        if (rc != 0) {
            klog(LOG_ERROR, "usb-msc", "READ(10) failed: LBA=%u count=%u",
                 (uint64_t)lba, (uint64_t)chunk);
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

    info = msc_state(dev);
    if (!info || !info->valid || info->sector_size == 0)
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

        /* exact_len=1: a short WRITE data phase must fail rather than report a
         * partial write as complete. */
        rc = msc_scsi_command(hc, dev, cdb, 10, (void *)in, data_len, 0, 1, NULL);
        if (rc != 0) {
            klog(LOG_ERROR, "usb-msc", "WRITE(10) failed: LBA=%u count=%u",
                 (uint64_t)lba, (uint64_t)chunk);
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
    struct usb_msc_info *info = msc_state(dev);
    if (!info)
        return NULL;
    return info->valid ? info : NULL;
}
