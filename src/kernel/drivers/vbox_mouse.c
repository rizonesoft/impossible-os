/* ============================================================================
 * vbox_mouse.c — VirtualBox VMMDev absolute mouse driver
 *
 * Discovers the VirtualBox Guest PCI device (0x80EE:0xCAFE), initializes
 * VMMDev protocol v1.03, and enables absolute mouse coordinates.
 *
 * Protocol overview (from OSDev wiki):
 *   1. Find PCI device (vendor 0x80EE, device 0xCAFE)
 *   2. BAR0 = I/O port base,  BAR1 = MMIO region (vmmdevmem)
 *   3. Send GuestInfo packet to announce ourselves (protocol v1.03)
 *   4. Send SetMouse packet with GUEST_CAN_ABSOLUTE flag
 *   5. Poll GetMouse packet each frame for absolute X, Y (0–0xFFFF)
 *   6. Scale to framebuffer resolution
 *
 * Buttons are NOT provided by VMMDev — PS/2 mouse still handles buttons.
 *
 * Reference: https://wiki.osdev.org/VirtualBox_Guest_Additions
 * ============================================================================ */

#include "kernel/drivers/vbox_mouse.h"
#include "kernel/drivers/pci.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/idt.h"
#include "kernel/drivers/pic.h"
#include "kernel/klog.h"
#include "kernel/mm/pmm.h"

/* ---- VMMDev constants ---- */

#define VBOX_VENDOR_ID              0x80EE
#define VBOX_DEVICE_ID              0xCAFE

#define VBOX_VMMDEV_VERSION         0x00010003  /* Protocol v1.03 (legacy) */
#define VBOX_REQUEST_HEADER_VERSION 0x10001

/* Request types */
#define VBOX_REQUEST_GET_MOUSE      1
#define VBOX_REQUEST_SET_MOUSE      2
#define VBOX_REQUEST_ACK_EVENTS     41
#define VBOX_REQUEST_GUEST_INFO     50

/* Mouse feature flags */
#define VBOX_MOUSE_GUEST_CAN_ABSOLUTE   (1u << 0)
#define VBOX_MOUSE_HOST_WANTS_ABSOLUTE  (1u << 1)
#define VBOX_MOUSE_NEW_PROTOCOL         (1u << 4)

/* Absolute coordinate range */
#define VBOX_ABS_MAX  0xFFFF

/* ---- VMMDev packet structures ---- */

struct vbox_header {
    uint32_t size;          /* Total packet size including header */
    uint32_t version;       /* Always VBOX_REQUEST_HEADER_VERSION */
    uint32_t request_type;  /* Request code */
    int32_t  rc;            /* Return code (filled by device) */
    uint32_t reserved1;
    uint32_t reserved2;
};

struct vbox_guest_info {
    struct vbox_header header;
    uint32_t version;       /* Protocol version */
    uint32_t ostype;        /* OS type (0 = unknown) */
};

struct vbox_mouse_absolute {
    struct vbox_header header;
    uint32_t features;      /* Capability flags */
    int32_t  x;             /* Absolute X (0–0xFFFF) */
    int32_t  y;             /* Absolute Y (0–0xFFFF) */
};

struct vbox_ack_events {
    struct vbox_header header;
    uint32_t events;        /* Events to acknowledge */
};

/* ---- Port I/O ---- */

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb_vbox(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ---- Driver state ---- */

static uint8_t  active;            /* 1 if VBox mouse is operational */
static uint16_t io_port;           /* BAR0: I/O port base */
static volatile uint32_t *vmmdev_mem;  /* BAR1: MMIO region */
static uint8_t  irq_line;         /* PCI IRQ line */

/* Packets — physically contiguous, page-aligned (identity-mapped via PMM) */
static struct vbox_mouse_absolute *mouse_pkt;
static uintptr_t mouse_pkt_phys;

static struct vbox_ack_events *ack_pkt;
static uintptr_t ack_pkt_phys;

/* Cached absolute position (updated by poll or IRQ) */
static volatile int32_t abs_x;
static volatile int32_t abs_y;

/* ---- Helpers ---- */

static void fill_header(struct vbox_header *h, uint32_t size, uint32_t type)
{
    h->size         = size;
    h->version      = VBOX_REQUEST_HEADER_VERSION;
    h->request_type = type;
    h->rc           = 0;
    h->reserved1    = 0;
    h->reserved2    = 0;
}

/* Send a packet to VMMDev by writing its physical address to the I/O port */
static void vbox_send(uintptr_t phys_addr)
{
    outl(io_port, (uint32_t)phys_addr);
}

/* Scale 0–0xFFFF to screen pixels */
static int32_t scale_abs(int32_t val, int32_t screen_max)
{
    if (val < 0) val = 0;
    if (val > VBOX_ABS_MAX) val = VBOX_ABS_MAX;
    return (int32_t)(((uint64_t)(uint32_t)val * (uint64_t)(screen_max - 1))
                     / (uint64_t)VBOX_ABS_MAX);
}

/* Poll for latest mouse position from VMMDev */
static void vbox_mouse_poll(void)
{
    /* Prepare a GetMouse request */
    fill_header(&mouse_pkt->header,
                sizeof(struct vbox_mouse_absolute),
                VBOX_REQUEST_GET_MOUSE);
    mouse_pkt->features = 0;
    mouse_pkt->x = 0;
    mouse_pkt->y = 0;

    vbox_send(mouse_pkt_phys);

    /* Check return code */
    if (mouse_pkt->header.rc >= 0) {
        /* Only update if host supports absolute mode */
        if (mouse_pkt->features & VBOX_MOUSE_HOST_WANTS_ABSOLUTE) {
            abs_x = scale_abs(mouse_pkt->x,
                              (int32_t)fb_get_width());
            abs_y = scale_abs(mouse_pkt->y,
                              (int32_t)fb_get_height());
        }
    }
}

/* ---- IRQ handler ---- */

static uint64_t vbox_irq_handler(struct interrupt_frame *frame)
{
    /* Check if there are pending events */
    if (vmmdev_mem && vmmdev_mem[2]) {
        /* Acknowledge all pending events */
        fill_header(&ack_pkt->header,
                    sizeof(struct vbox_ack_events),
                    VBOX_REQUEST_ACK_EVENTS);
        ack_pkt->events = vmmdev_mem[2];
        vbox_send(ack_pkt_phys);

        /* Poll mouse position */
        vbox_mouse_poll();
    }

    pic_send_eoi(irq_line);
    return (uint64_t)frame;
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

int vbox_mouse_init(void)
{
    struct pci_device pci;
    uintptr_t pkt_page;

    active = 0;
    abs_x = 0;
    abs_y = 0;

    /* Find VBox Guest PCI device (vendor 0x80EE, device 0xCAFE) */
    pci = pci_find_device(VBOX_VENDOR_ID, VBOX_DEVICE_ID);
    if (!pci.found) {
        klog(LOG_DEBUG, "vbox", "VBoxGuest PCI device not found (not VirtualBox)");
        return -1;
    }

    /* BAR0 = I/O port base (mask low 2 bits for I/O space) */
    io_port = (uint16_t)(pci.bar[0] & 0xFFFFFFFC);

    /* BAR1 = MMIO region (mask low 4 bits for memory space)
     * Identity-mapped in our kernel, so physical == virtual. */
    vmmdev_mem = (volatile uint32_t *)(uintptr_t)(pci.bar[1] & 0xFFFFFFF0);

    irq_line = pci.irq_line;

    klog(LOG_INFO, "vbox", "VBoxGuest found: I/O 0x%x, MMIO 0x%x, IRQ %u",
         (uint64_t)io_port, (uint64_t)(uintptr_t)vmmdev_mem,
         (uint64_t)irq_line);

    /* Enable PCI I/O space + memory space + bus mastering */
    pci_enable_bus_mastering(&pci);
    {
        uint16_t cmd = pci_read16(pci.bus, pci.dev, pci.func, PCI_COMMAND);
        cmd |= PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE;
        pci_write16(pci.bus, pci.dev, pci.func, PCI_COMMAND, cmd);
    }

    /* Allocate a page for VMMDev packets.
     * We fit all packets in one 4096-byte page.
     * Identity-mapped, so phys == virt. */
    pkt_page = pmm_alloc_contiguous(1);
    if (!pkt_page) {
        klog(LOG_ERROR, "vbox", "Failed to allocate packet page");
        return -1;
    }

    /* Zero the page */
    {
        uint8_t *p = (uint8_t *)pkt_page;
        uint32_t i;
        for (i = 0; i < 4096; i++)
            p[i] = 0;
    }

    /* Layout packets within the page */
    mouse_pkt      = (struct vbox_mouse_absolute *)pkt_page;
    mouse_pkt_phys = pkt_page;

    ack_pkt      = (struct vbox_ack_events *)(pkt_page + 256);
    ack_pkt_phys = pkt_page + 256;

    /* Send GuestInfo to announce ourselves (protocol v1.03) */
    {
        struct vbox_guest_info *info =
            (struct vbox_guest_info *)(pkt_page + 512);
        uintptr_t info_phys = pkt_page + 512;

        fill_header(&info->header,
                    sizeof(struct vbox_guest_info),
                    VBOX_REQUEST_GUEST_INFO);
        info->version = VBOX_VMMDEV_VERSION;
        info->ostype  = 0;  /* Unknown OS */

        vbox_send(info_phys);

        if (info->header.rc < 0) {
            klog(LOG_ERROR, "vbox", "GuestInfo rejected (rc=%d)",
                 (uint64_t)(uint32_t)info->header.rc);
            return -1;
        }
    }

    /* Enable absolute mouse: send SetMouse with capability flags */
    {
        fill_header(&mouse_pkt->header,
                    sizeof(struct vbox_mouse_absolute),
                    VBOX_REQUEST_SET_MOUSE);
        mouse_pkt->features = VBOX_MOUSE_GUEST_CAN_ABSOLUTE
                            | VBOX_MOUSE_NEW_PROTOCOL;
        mouse_pkt->x = 0;
        mouse_pkt->y = 0;

        vbox_send(mouse_pkt_phys);

        if (mouse_pkt->header.rc < 0) {
            klog(LOG_ERROR, "vbox", "SetMouse failed (rc=%d)",
                 (uint64_t)(uint32_t)mouse_pkt->header.rc);
            return -1;
        }
    }

    /* Initialize position at screen center */
    abs_x = (int32_t)(fb_get_width() / 2);
    abs_y = (int32_t)(fb_get_height() / 2);

    /* Register IRQ handler */
    idt_register_handler(PIC1_OFFSET + irq_line, vbox_irq_handler);
    pic_unmask_irq(irq_line);

    /* Enable all VMMDev interrupts */
    if (vmmdev_mem)
        vmmdev_mem[3] = 0xFFFFFFFF;

    active = 1;
    klog(LOG_INFO, "vbox", "VBox absolute mouse enabled (IRQ %u)",
         (uint64_t)irq_line);

    return 0;
}

/* ============================================================================
 * State query
 * ============================================================================ */

struct mouse_state vbox_mouse_get_state(void)
{
    struct mouse_state s;

    /* Poll for latest position */
    if (active)
        vbox_mouse_poll();

    s.x = abs_x;
    s.y = abs_y;
    s.buttons = 0;  /* VMMDev does NOT provide buttons — merge with PS/2 */
    return s;
}

uint8_t vbox_mouse_available(void)
{
    return active;
}
