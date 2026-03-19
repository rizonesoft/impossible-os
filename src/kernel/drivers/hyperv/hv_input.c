/* ============================================================================
 * hv_input.c — Hyper-V Synthetic HID Input (Keyboard + Mouse)
 *
 * Clean-room implementation from Hyper-V TLFS (public spec).
 *
 * The synthetic HID protocol over VMBus:
 *   1. Open channel by GUID
 *   2. Send PROTOCOL_REQUEST with version
 *   3. Receive PROTOCOL_RESPONSE (accepted=1)
 *   4. Receive INITIAL_DEVICE_INFO
 *   5. Send INITIAL_DEVICE_INFO_ACK
 *   6. Poll for INPUT_REPORT messages containing HID reports
 *
 * Keyboard reports: single byte = PS/2 scan code (set 1)
 * Mouse reports: { buttons(1), x_abs_lo(1), x_abs_hi(1), y_abs_lo(1), y_abs_hi(1) }
 * ============================================================================ */

#include "kernel/drivers/hyperv/hv_input.h"
#include "kernel/drivers/hyperv/vmbus.h"
#include "kernel/drivers/keyboard.h"
#include "kernel/drivers/mouse.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/klog.h"
#include "kernel/printk.h"

/* ---- Local helpers ---- */

static void hv_memset(void *dst, uint8_t val, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--)
        *d++ = val;
}

/* ---- State ---- */

static struct vmbus_channel *kbd_channel;
static struct vmbus_channel *mouse_channel;
static int kbd_active;
static int mouse_active;

#define HV_INPUT_RING_PAGES   16  /* 8 send + 8 recv = 32 KiB each */

/* ---- Protocol negotiation ---- */

static int hv_hid_negotiate(struct vmbus_channel *ch, const char *name)
{
    struct hv_hid_protocol_request req;
    uint8_t resp_buf[256];
    uint32_t bytes;
    uint32_t attempts;

    /* Send protocol version request */
    hv_memset(&req, 0, sizeof(req));
    req.header.type = HV_HID_PROTOCOL_REQUEST;
    req.header.size = sizeof(req);
    req.version = HV_HID_VERSION_WIN10;

    if (vmbus_ring_write(ch, &req, sizeof(req)) < 0) {
        klog(LOG_ERROR, "hv_input", "%s: ring write failed", name);
        return -1;
    }
    vmbus_signal_channel(ch);

    /* Wait for PROTOCOL_RESPONSE */
    for (attempts = 0; attempts < 50000; attempts++) {
        __asm__ volatile("pause" ::: "memory");
        bytes = vmbus_ring_read(ch, resp_buf, sizeof(resp_buf));
        if (bytes >= sizeof(struct hv_hid_protocol_response)) {
            struct hv_hid_protocol_response *resp =
                (struct hv_hid_protocol_response *)resp_buf;
            if (resp->header.type == HV_HID_PROTOCOL_RESPONSE) {
                if (!resp->accepted) {
                    klog(LOG_ERROR, "hv_input",
                         "%s: protocol version rejected", name);
                    return -1;
                }
                klog(LOG_DEBUG, "hv_input",
                     "%s: protocol negotiated", name);
                break;
            }
        }
    }

    /* Wait for INITIAL_DEVICE_INFO */
    for (attempts = 0; attempts < 50000; attempts++) {
        __asm__ volatile("pause" ::: "memory");
        bytes = vmbus_ring_read(ch, resp_buf, sizeof(resp_buf));
        if (bytes >= sizeof(struct hv_hid_msg_header)) {
            struct hv_hid_msg_header *hdr =
                (struct hv_hid_msg_header *)resp_buf;
            if (hdr->type == HV_HID_INITIAL_DEVICE_INFO) {
                klog(LOG_DEBUG, "hv_input",
                     "%s: device info received", name);
                break;
            }
        }
    }

    /* Send INITIAL_DEVICE_INFO_ACK */
    struct hv_hid_msg_header ack;
    hv_memset(&ack, 0, sizeof(ack));
    ack.type = HV_HID_INITIAL_DEVICE_INFO_ACK;
    ack.size = sizeof(ack);

    if (vmbus_ring_write(ch, &ack, sizeof(ack)) < 0) {
        klog(LOG_ERROR, "hv_input", "%s: ACK write failed", name);
        return -1;
    }
    vmbus_signal_channel(ch);

    klog(LOG_INFO, "hv_input", "%s: initialization complete", name);
    return 0;
}

/* ---- Keyboard ---- */

int hv_kbd_init(void)
{
    struct hv_guid kbd_guid = HV_GUID_KBD_VSP;

    if (vmbus_get_channel_count() <= 0)
        return -1;

    kbd_channel = vmbus_find_channel_by_guid(&kbd_guid);
    if (!kbd_channel) {
        klog(LOG_DEBUG, "hv_input", "Keyboard VSP channel not found");
        return -1;
    }

    klog(LOG_INFO, "hv_input",
         "Found keyboard VSP (relid=%u)",
         (uint32_t)kbd_channel->child_relid);

    if (vmbus_open_channel(kbd_channel, HV_INPUT_RING_PAGES) < 0) {
        klog(LOG_ERROR, "hv_input", "Failed to open keyboard channel");
        return -1;
    }

    if (hv_hid_negotiate(kbd_channel, "kbd") < 0)
        return -1;

    kbd_active = 1;
    printk("[OK] HV Keyboard: synthetic input active\n");
    return 0;
}

int hv_kbd_available(void)
{
    return kbd_active;
}

void hv_kbd_poll(void)
{
    uint8_t buf[256];
    uint32_t bytes;

    if (!kbd_active || !kbd_channel)
        return;

    bytes = vmbus_ring_read(kbd_channel, buf, sizeof(buf));
    if (bytes < sizeof(struct hv_hid_msg_header))
        return;

    struct hv_hid_msg_header *hdr = (struct hv_hid_msg_header *)buf;

    if (hdr->type != HV_HID_INPUT_REPORT)
        return;

    /* Input report data starts after the header.
     * For keyboard, each byte after the header is a PS/2 scan code. */
    uint32_t data_offset = sizeof(struct hv_hid_msg_header);
    uint32_t data_len = bytes - data_offset;
    uint32_t i;

    for (i = 0; i < data_len; i++) {
        keyboard_inject_scancode(buf[data_offset + i]);
    }
}

/* ---- Mouse ---- */

int hv_mouse_init(void)
{
    struct hv_guid mouse_guid = HV_GUID_MOUSE_VSP;

    if (vmbus_get_channel_count() <= 0)
        return -1;

    mouse_channel = vmbus_find_channel_by_guid(&mouse_guid);
    if (!mouse_channel) {
        klog(LOG_DEBUG, "hv_input", "Mouse VSP channel not found");
        return -1;
    }

    klog(LOG_INFO, "hv_input",
         "Found mouse VSP (relid=%u)",
         (uint32_t)mouse_channel->child_relid);

    if (vmbus_open_channel(mouse_channel, HV_INPUT_RING_PAGES) < 0) {
        klog(LOG_ERROR, "hv_input", "Failed to open mouse channel");
        return -1;
    }

    if (hv_hid_negotiate(mouse_channel, "mouse") < 0)
        return -1;

    mouse_active = 1;
    printk("[OK] HV Mouse: synthetic input active\n");
    return 0;
}

int hv_mouse_available(void)
{
    return mouse_active;
}

void hv_mouse_poll(void)
{
    uint8_t buf[256];
    uint32_t bytes;

    if (!mouse_active || !mouse_channel)
        return;

    bytes = vmbus_ring_read(mouse_channel, buf, sizeof(buf));
    if (bytes < sizeof(struct hv_hid_msg_header))
        return;

    struct hv_hid_msg_header *hdr = (struct hv_hid_msg_header *)buf;

    if (hdr->type != HV_HID_INPUT_REPORT)
        return;

    /* Mouse HID report (after header):
     *   Byte 0: buttons (bit0=left, bit1=right, bit2=middle)
     *   Byte 1-2: X absolute (little-endian, 0-65535)
     *   Byte 3-4: Y absolute (little-endian, 0-65535) */
    uint32_t data_offset = sizeof(struct hv_hid_msg_header);
    uint32_t data_len = bytes - data_offset;

    if (data_len < 5)
        return;

    uint8_t buttons = buf[data_offset];
    uint16_t abs_x = (uint16_t)buf[data_offset + 1] |
                     ((uint16_t)buf[data_offset + 2] << 8);
    uint16_t abs_y = (uint16_t)buf[data_offset + 3] |
                     ((uint16_t)buf[data_offset + 4] << 8);

    /* Scale from 0-65535 to framebuffer resolution */
    int32_t px = (int32_t)((uint32_t)abs_x * fb_get_width() / 65536);
    int32_t py = (int32_t)((uint32_t)abs_y * fb_get_height() / 65536);

    mouse_inject_state(px, py, buttons);
}
