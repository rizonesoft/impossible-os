/* ============================================================================
 * virtio_input.c — VirtIO Input device driver (tablet / absolute mouse)
 *
 * Discovers a virtio-input PCI device (vendor 0x1AF4, device 0x1052),
 * initialises the event virtqueue via the modern PCI transport, and
 * processes Linux evdev-style events delivered via VirtIO.
 *
 * Absolute X/Y values are scaled from the device range (0-32767) to
 * framebuffer resolution.
 *
 * The device has two virtqueues:
 *   VQ 0 — eventq:  device → driver (input events)
 *   VQ 1 — statusq: driver → device (LED status, unused here)
 *
 * We pre-fill eventq with receive buffers. The device writes events into
 * them and returns them via the used ring. We process the events and
 * recycle the buffers.
 * ============================================================================ */

#include "kernel/drivers/virtio_input.h"
#include "kernel/drivers/virtio.h"
#include "kernel/drivers/pci.h"
#include "kernel/idt.h"
#include "kernel/drivers/pic.h"
#include "kernel/printk.h"
#include "kernel/mm/heap.h"
#include "kernel/drivers/mouse.h"
#include "kernel/drivers/framebuffer.h"

/* ---- VirtIO tablet axis range (QEMU default) ---- */
#define VIRTIO_ABS_MAX  32767

/* ---- Event receive buffer pool ---- */
#define EVENT_BUF_COUNT  64

/* ---- Driver state ---- */
static struct virtio_pci_dev pci_dev;   /* PCI MMIO bases */
static uint8_t  irq_line;              /* PCI IRQ line */
static uint8_t  active;                /* 1 if driver is operational */

static struct virtqueue eventq;         /* VQ 0: device → driver events */

/* Pre-allocated event buffers.  Each holds one virtio_input_event. */
static struct virtio_input_event event_bufs[EVENT_BUF_COUNT]
    __attribute__((aligned(8)));

/* Current absolute mouse state (updated by IRQ handler or poll) */
static volatile int32_t abs_x;
static volatile int32_t abs_y;
static volatile uint8_t btn_state;     /* MOUSE_BTN_LEFT etc. */

/* Pending absolute values before SYN_REPORT */
static volatile int32_t pending_x;
static volatile int32_t pending_y;
static volatile uint8_t pending_btn;
static volatile uint8_t has_pending;

/* ---- Helpers ---- */

/* Scale a 0-32767 absolute value to screen pixels */
static int32_t scale_abs(int32_t val, int32_t screen_max)
{
    if (val < 0) val = 0;
    if (val > VIRTIO_ABS_MAX) val = VIRTIO_ABS_MAX;

    return (int32_t)(((uint64_t)val * (uint64_t)(screen_max - 1))
                     / (uint64_t)VIRTIO_ABS_MAX);
}

/* Process one evdev event */
static void process_event(const struct virtio_input_event *ev)
{
    switch (ev->type) {
    case EV_ABS:
        if (ev->code == ABS_X) {
            pending_x = scale_abs((int32_t)ev->value,
                                  (int32_t)fb_get_width());
            has_pending = 1;
        } else if (ev->code == ABS_Y) {
            pending_y = scale_abs((int32_t)ev->value,
                                  (int32_t)fb_get_height());
            has_pending = 1;
        }
        break;

    case EV_KEY:
        if (ev->code == BTN_LEFT) {
            if (ev->value)
                pending_btn |= MOUSE_BTN_LEFT;
            else
                pending_btn &= (uint8_t)~MOUSE_BTN_LEFT;
            has_pending = 1;
        } else if (ev->code == BTN_RIGHT) {
            if (ev->value)
                pending_btn |= MOUSE_BTN_RIGHT;
            else
                pending_btn &= (uint8_t)~MOUSE_BTN_RIGHT;
            has_pending = 1;
        } else if (ev->code == BTN_MIDDLE) {
            if (ev->value)
                pending_btn |= MOUSE_BTN_MIDDLE;
            else
                pending_btn &= (uint8_t)~MOUSE_BTN_MIDDLE;
            has_pending = 1;
        }
        break;

    case EV_SYN:
        if (ev->code == SYN_REPORT && has_pending) {
            /* Commit pending state atomically */
            abs_x     = pending_x;
            abs_y     = pending_y;
            btn_state = pending_btn;
            has_pending = 0;
        }
        break;

    default:
        break;
    }
}

/* Drain all completed events from the used ring */
static void drain_eventq(void)
{
    uint32_t len;
    int idx;

    while ((idx = virtq_get_buf(&eventq, &len)) >= 0) {
        if (idx < EVENT_BUF_COUNT && len >= sizeof(struct virtio_input_event))
            process_event(&event_bufs[idx]);

        /* Recycle the buffer: re-add to available ring */
        virtq_free_desc(&eventq, (uint16_t)idx);
        virtq_add_buf(&eventq, &event_bufs[idx],
                      sizeof(struct virtio_input_event),
                      VIRTQ_DESC_F_WRITE);
    }

    /* Notify device that we refilled buffers */
    virtq_kick(&eventq);
}

/* ---- IRQ handler ---- */
static uint64_t virtio_input_irq(struct interrupt_frame *frame)
{
    /* Read ISR status to acknowledge the interrupt (modern: MMIO read) */
    virtio_read_isr(&pci_dev);

    /* Process events */
    drain_eventq();

    pic_send_eoi(irq_line);
    return (uint64_t)frame;
}

/* ---- Public API ---- */

int virtio_input_init(void)
{
    struct pci_device pci;
    int i;

    active = 0;
    abs_x = (int32_t)fb_get_width() / 2;
    abs_y = (int32_t)fb_get_height() / 2;
    btn_state = 0;
    pending_x = abs_x;
    pending_y = abs_y;
    pending_btn = 0;
    has_pending = 0;

    /* Find the VirtIO input device on PCI bus (modern ID) */
    pci = pci_find_device(VIRTIO_PCI_VENDOR, VIRTIO_PCI_DEV_INPUT);
    if (!pci.found) {
        printk("[VIRTIO-INPUT] No virtio-input device found\n");
        return -1;
    }

    irq_line = pci.irq_line;

    printk("[VIRTIO-INPUT] Found at PCI %u:%u.%u, IRQ %u\n",
           (uint64_t)pci.bus, (uint64_t)pci.dev, (uint64_t)pci.func,
           (uint64_t)irq_line);

    /* Enable PCI bus mastering + memory space */
    pci_enable_bus_mastering(&pci);

    /* ---- Discover VirtIO MMIO regions via PCI capabilities ---- */
    if (virtio_pci_init(&pci_dev, pci.bus, pci.dev, pci.func) < 0) {
        printk("[VIRTIO-INPUT] Failed to parse PCI capabilities\n");
        return -1;
    }

    /* ---- VirtIO initialization sequence (spec §3.1) ---- */

    /* 1. Reset device */
    virtio_set_status(&pci_dev, 0);

    /* 2. Acknowledge the device */
    virtio_set_status(&pci_dev, VIRTIO_STATUS_ACKNOWLEDGE);

    /* 3. Set DRIVER bit */
    virtio_set_status(&pci_dev,
                      VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* 4. Skip feature negotiation for now (accept no features).
     *    Select feature page 0, write 0 to driver features. */
    {
        volatile uint8_t *cfg = pci_dev.common_cfg;
        volatile uint32_t *gfsel = (volatile uint32_t *)(cfg +
                                    VIRTIO_COMMON_GFSELECT);
        volatile uint32_t *gf = (volatile uint32_t *)(cfg +
                                  VIRTIO_COMMON_GF);
        *gfsel = 0;
        *gf    = 0;
        *gfsel = 1;
        *gf    = 0;
    }

    /* 5. Set FEATURES_OK */
    virtio_set_status(&pci_dev,
                      VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                      VIRTIO_STATUS_FEATURES_OK);

    /* Verify FEATURES_OK is still set */
    if (!(virtio_get_status(&pci_dev) & VIRTIO_STATUS_FEATURES_OK)) {
        printk("[VIRTIO-INPUT] Device did not accept features\n");
        virtio_set_status(&pci_dev, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* 6. Set up eventq (VQ 0) */
    if (virtq_init(&eventq, &pci_dev, 0) < 0) {
        printk("[VIRTIO-INPUT] Failed to init eventq\n");
        virtio_set_status(&pci_dev, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* 7. Pre-fill eventq with receive buffers */
    for (i = 0; i < EVENT_BUF_COUNT && i < eventq.size; i++) {
        virtq_add_buf(&eventq, &event_bufs[i],
                      sizeof(struct virtio_input_event),
                      VIRTQ_DESC_F_WRITE);
    }
    virtq_kick(&eventq);

    /* 8. Register IRQ handler */
    idt_register_handler(PIC1_OFFSET + irq_line, virtio_input_irq);
    pic_unmask_irq(irq_line);

    /* 9. Set DRIVER_OK — device is live */
    virtio_set_status(&pci_dev,
                      VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                      VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    active = 1;
    printk("[OK] VirtIO input initialized (tablet mode, IRQ %u)\n",
           (uint64_t)irq_line);

    return 0;
}

struct virtio_input_state virtio_input_get_state(void)
{
    struct virtio_input_state s;

    /* Poll for any pending events (in case IRQ was missed) */
    if (active)
        drain_eventq();

    s.x       = abs_x;
    s.y       = abs_y;
    s.buttons = btn_state;
    return s;
}

uint8_t virtio_input_available(void)
{
    return active;
}
