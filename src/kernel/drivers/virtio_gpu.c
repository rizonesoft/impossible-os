/* ============================================================================
 * virtio_gpu.c — VirtIO-GPU driver (cursor-only mode)
 *
 * With VGA handling all display output (1280×720, VBE page flip), the
 * VirtIO-GPU device (max_outputs=0) is used exclusively for the hardware
 * cursor plane:
 *
 *   - Cursor image uploaded via controlq (CREATE_2D → ATTACH → TRANSFER)
 *   - Cursor position updated via cursorq (UPDATE_CURSOR / MOVE_CURSOR)
 *
 * This gives instant, GPU-composited cursor movement independent of the
 * compositor's frame rate, with zero double-cursor artifacts.
 * ============================================================================ */

#include "kernel/drivers/virtio_gpu.h"
#include "kernel/drivers/virtio.h"
#include "kernel/drivers/pci.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/idt.h"
#include "kernel/drivers/pic.h"
#include "kernel/printk.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"

/* ---- Resource IDs ---- */
#define RES_CURSOR       1   /* Hardware cursor image */

/* ---- Cursor dimensions ---- */
#define CURSOR_W  64
#define CURSOR_H  64

/* ---- Driver state ---- */
static struct virtio_pci_dev gpu_pci;
static struct virtqueue controlq;   /* VQ 0: control commands */
static struct virtqueue cursorq;    /* VQ 1: cursor commands */
static uint8_t  gpu_irq;
static uint8_t  gpu_active;

/* Command/response buffer for controlq (properly aligned) */
static struct virtio_gpu_ctrl_hdr ctrl_resp __attribute__((aligned(16)));

/* Cursor command buffer (cursor queue uses single-descriptor, no response) */
static struct virtio_gpu_update_cursor cursor_cmd __attribute__((aligned(16)));

/* Backing entry for ATTACH_BACKING (follows the command struct in memory) */
struct attach_backing_with_entry {
    struct virtio_gpu_resource_attach_backing cmd;
    struct virtio_gpu_mem_entry entry;
} __attribute__((packed));

static struct attach_backing_with_entry attach_cmd __attribute__((aligned(16)));

/* ---- Helpers ---- */

static void gpu_memset(void *dst, uint8_t val, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = val;
}

static void gpu_memcpy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

/* Send a control command and wait for the response.
 * Returns the response type, or -1 on timeout. */
static int gpu_ctrl_send(void *cmd_buf, uint32_t cmd_len)
{
    int head;
    uint32_t len;

    ctrl_resp.type = 0;

    head = virtq_add_buf_chain(&controlq,
                               cmd_buf, cmd_len,
                               &ctrl_resp, sizeof(ctrl_resp));
    if (head < 0)
        return -1;

    virtq_kick(&controlq);

    /* Spin-wait for completion (only used during init, not hot path) */
    int timeout = 1000000;
    while (virtq_get_buf(&controlq, &len) < 0) {
        __asm__ volatile("pause");
        if (--timeout <= 0) {
            printk("[VIRTIO-GPU] Command 0x%x timed out\n",
                   (uint64_t)((struct virtio_gpu_ctrl_hdr *)cmd_buf)->type);
            return -1;
        }
    }

    /* Free both descriptors in the chain */
    uint16_t tail = controlq.desc[head].next;
    virtq_free_desc(&controlq, (uint16_t)head);
    virtq_free_desc(&controlq, tail);

    return (int)ctrl_resp.type;
}

/* ---- IRQ handler ---- */
static uint64_t virtio_gpu_irq(struct interrupt_frame *frame)
{
    virtio_read_isr(&gpu_pci);
    uint32_t len;
    while (virtq_get_buf(&controlq, &len) >= 0) { }
    pic_send_eoi(gpu_irq);
    return (uint64_t)frame;
}

/* ---- Public API ---- */

int virtio_gpu_init(void)
{
    struct pci_device pci;
    int resp;

    gpu_active = 0;

    /* Find VirtIO-GPU on PCI bus */
    pci = pci_find_device(VIRTIO_PCI_VENDOR, VIRTIO_PCI_DEV_GPU);
    if (!pci.found) {
        printk("[VIRTIO-GPU] No virtio-gpu device found\n");
        return -1;
    }

    gpu_irq = pci.irq_line;
    printk("[VIRTIO-GPU] Found at PCI %u:%u.%u, IRQ %u\n",
           (uint64_t)pci.bus, (uint64_t)pci.dev, (uint64_t)pci.func,
           (uint64_t)gpu_irq);

    pci_enable_bus_mastering(&pci);

    /* Parse PCI capabilities for MMIO regions */
    if (virtio_pci_init(&gpu_pci, pci.bus, pci.dev, pci.func) < 0) {
        printk("[VIRTIO-GPU] Failed to parse PCI capabilities\n");
        return -1;
    }

    /* ---- VirtIO init sequence ---- */
    virtio_set_status(&gpu_pci, 0);  /* Reset */
    virtio_set_status(&gpu_pci, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_set_status(&gpu_pci, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Accept no features (minimal driver) */
    {
        volatile uint8_t *cfg = gpu_pci.common_cfg;
        volatile uint32_t *gfsel = (volatile uint32_t *)(cfg + VIRTIO_COMMON_GFSELECT);
        volatile uint32_t *gf = (volatile uint32_t *)(cfg + VIRTIO_COMMON_GF);
        *gfsel = 0; *gf = 0;
        *gfsel = 1; *gf = 0;
    }

    virtio_set_status(&gpu_pci,
                      VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                      VIRTIO_STATUS_FEATURES_OK);

    if (!(virtio_get_status(&gpu_pci) & VIRTIO_STATUS_FEATURES_OK)) {
        printk("[VIRTIO-GPU] Features not accepted\n");
        virtio_set_status(&gpu_pci, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* Init controlq (VQ 0) and cursorq (VQ 1) */
    if (virtq_init(&controlq, &gpu_pci, 0) < 0) {
        printk("[VIRTIO-GPU] Failed to init controlq\n");
        return -1;
    }
    if (virtq_init(&cursorq, &gpu_pci, 1) < 0) {
        printk("[VIRTIO-GPU] Failed to init cursorq\n");
        return -1;
    }

    /* NOTE: Do NOT register IRQ handler yet — the polling-based init
     * commands below would race with the IRQ handler that drains
     * the used ring.  Register after all init commands are done. */

    /* Set DRIVER_OK — device is live */
    virtio_set_status(&gpu_pci,
                      VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                      VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    /* ---- Create cursor resource (64×64 BGRA) ---- */
    {
        struct virtio_gpu_resource_create_2d cmd;
        gpu_memset(&cmd, 0, sizeof(cmd));
        cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
        cmd.resource_id = RES_CURSOR;
        cmd.format      = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
        cmd.width       = CURSOR_W;
        cmd.height      = CURSOR_H;

        resp = gpu_ctrl_send(&cmd, sizeof(cmd));
        if (resp != VIRTIO_GPU_RESP_OK_NODATA) {
            printk("[VIRTIO-GPU] RESOURCE_CREATE_2D (cursor) failed (0x%x)\n",
                   (uint64_t)resp);
            return -1;
        }
    }

    /* Now safe to register IRQ handler (all polling-based init done) */
    idt_register_handler(PIC1_OFFSET + gpu_irq, virtio_gpu_irq);
    pic_unmask_irq(gpu_irq);

    gpu_active = 1;
    printk("[OK] VirtIO-GPU hw cursor initialized\n");

    return 0;
}

uint8_t virtio_gpu_available(void)
{
    return gpu_active;
}

/* ---- Display flush: VGA handles all display via VBE page flip ---- */

void virtio_gpu_flush_rect(uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height)
{
    (void)x; (void)y; (void)width; (void)height;
}

void virtio_gpu_flush(void)
{
}

/* ---- Hardware cursor ---- */

void virtio_gpu_set_cursor(const uint32_t *pixels,
                           uint32_t width, uint32_t height,
                           uint32_t hot_x, uint32_t hot_y)
{
    if (!gpu_active)
        return;

    /* Build a zero-padded 64×64 cursor image */
    uint32_t cursor_buf[CURSOR_W * CURSOR_H];
    uint32_t row;

    gpu_memset(cursor_buf, 0, sizeof(cursor_buf));

    uint32_t copy_w = (width > CURSOR_W) ? CURSOR_W : width;
    uint32_t copy_h = (height > CURSOR_H) ? CURSOR_H : height;
    for (row = 0; row < copy_h; row++) {
        gpu_memcpy(&cursor_buf[row * CURSOR_W],
                   &pixels[row * width],
                   copy_w * 4);
    }

    /* Attach backing to cursor resource */
    {
        gpu_memset(&attach_cmd, 0, sizeof(attach_cmd));
        attach_cmd.cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
        attach_cmd.cmd.resource_id = RES_CURSOR;
        attach_cmd.cmd.nr_entries  = 1;
        attach_cmd.entry.addr      = (uint64_t)cursor_buf;
        attach_cmd.entry.length    = CURSOR_W * CURSOR_H * 4;

        gpu_ctrl_send(&attach_cmd,
                      sizeof(struct virtio_gpu_resource_attach_backing) +
                      sizeof(struct virtio_gpu_mem_entry));
    }

    /* Transfer cursor data to GPU */
    {
        struct virtio_gpu_transfer_to_host_2d cmd;
        gpu_memset(&cmd, 0, sizeof(cmd));
        cmd.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
        cmd.r.x         = 0;
        cmd.r.y         = 0;
        cmd.r.width     = CURSOR_W;
        cmd.r.height    = CURSOR_H;
        cmd.offset      = 0;
        cmd.resource_id = RES_CURSOR;

        gpu_ctrl_send(&cmd, sizeof(cmd));
    }

    /* Send UPDATE_CURSOR on the cursor queue */
    gpu_memset(&cursor_cmd, 0, sizeof(cursor_cmd));
    cursor_cmd.hdr.type       = VIRTIO_GPU_CMD_UPDATE_CURSOR;
    cursor_cmd.pos.scanout_id = 0;
    cursor_cmd.pos.x          = 0;
    cursor_cmd.pos.y          = 0;
    cursor_cmd.resource_id    = RES_CURSOR;
    cursor_cmd.hot_x          = hot_x;
    cursor_cmd.hot_y          = hot_y;

    virtq_add_buf(&cursorq, &cursor_cmd, sizeof(cursor_cmd), 0);
    virtq_kick(&cursorq);
}

void virtio_gpu_move_cursor(uint32_t x, uint32_t y)
{
    if (!gpu_active)
        return;

    cursor_cmd.hdr.type       = VIRTIO_GPU_CMD_MOVE_CURSOR;
    cursor_cmd.pos.scanout_id = 0;
    cursor_cmd.pos.x          = x;
    cursor_cmd.pos.y          = y;

    /* Drain any previous cursor command from the used ring */
    {
        uint32_t len;
        int idx;
        while ((idx = virtq_get_buf(&cursorq, &len)) >= 0)
            virtq_free_desc(&cursorq, (uint16_t)idx);
    }

    virtq_add_buf(&cursorq, &cursor_cmd, sizeof(cursor_cmd), 0);
    virtq_kick(&cursorq);
}

void virtio_gpu_hide_cursor(void)
{
    if (!gpu_active)
        return;

    gpu_memset(&cursor_cmd, 0, sizeof(cursor_cmd));
    cursor_cmd.hdr.type       = VIRTIO_GPU_CMD_UPDATE_CURSOR;
    cursor_cmd.pos.scanout_id = 0;
    cursor_cmd.resource_id    = 0;  /* resource_id=0 hides cursor */

    {
        uint32_t len;
        int idx;
        while ((idx = virtq_get_buf(&cursorq, &len)) >= 0)
            virtq_free_desc(&cursorq, (uint16_t)idx);
    }

    virtq_add_buf(&cursorq, &cursor_cmd, sizeof(cursor_cmd), 0);
    virtq_kick(&cursorq);
}
