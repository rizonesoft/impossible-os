/* ============================================================================
 * virtio_gpu.c — VirtIO-GPU driver (P18-lite: hardware cursor + scanout)
 *
 * Minimal VirtIO-GPU 2D driver:
 *   - Creates a framebuffer resource → SET_SCANOUT → ATTACH_BACKING
 *   - Provides flush (TRANSFER_TO_HOST_2D + RESOURCE_FLUSH) to replace fb_swap
 *   - Hardware cursor via UPDATE_CURSOR / MOVE_CURSOR on cursor virtqueue
 *
 * The cursor queue (VQ 1) is processed independently by the GPU, so cursor
 * moves happen at display refresh rate regardless of compositor speed.
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
#define RES_FRAMEBUFFER  1   /* Main display framebuffer */
#define RES_CURSOR       2   /* Hardware cursor image */

/* ---- Cursor dimensions ---- */
#define CURSOR_W  64
#define CURSOR_H  64

/* ---- Driver state ---- */
static struct virtio_pci_dev gpu_pci;
static struct virtqueue controlq;   /* VQ 0: control commands */
static struct virtqueue cursorq;    /* VQ 1: cursor commands */
static uint8_t  gpu_irq;
static uint8_t  gpu_active;

/* Display dimensions (from framebuffer) */
static uint32_t disp_width;
static uint32_t disp_height;

/* Backing memory for the framebuffer resource — points to fb back buffer */
static uint32_t *fb_backing;
static uint32_t  fb_backing_size;

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
 * cmd_buf must contain the packed command, cmd_len = its size.
 * Returns the response type, or -1 on error. */
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

/* Send a control command with a larger response buffer */
static int gpu_ctrl_send_resp(void *cmd_buf, uint32_t cmd_len,
                              void *resp_buf, uint32_t resp_len)
{
    int head;
    uint32_t len;

    head = virtq_add_buf_chain(&controlq,
                               cmd_buf, cmd_len,
                               resp_buf, resp_len);
    if (head < 0)
        return -1;

    virtq_kick(&controlq);

    int timeout = 1000000;
    while (virtq_get_buf(&controlq, &len) < 0) {
        __asm__ volatile("pause");
        if (--timeout <= 0)
            return -1;
    }

    uint16_t tail = controlq.desc[head].next;
    virtq_free_desc(&controlq, (uint16_t)head);
    virtq_free_desc(&controlq, tail);

    return (int)((struct virtio_gpu_ctrl_hdr *)resp_buf)->type;
}

/* ---- IRQ handler ---- */
static uint64_t virtio_gpu_irq(struct interrupt_frame *frame)
{
    virtio_read_isr(&gpu_pci);
    /* Drain completed control commands (if any async ones) */
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

    /* ---- Get display info (verify GPU is responsive) ---- */
    {
        struct virtio_gpu_ctrl_hdr get_info;
        struct virtio_gpu_resp_display_info display_info;

        gpu_memset(&get_info, 0, sizeof(get_info));
        get_info.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

        gpu_memset(&display_info, 0, sizeof(display_info));

        resp = gpu_ctrl_send_resp(&get_info, sizeof(get_info),
                                  &display_info, sizeof(display_info));
        if (resp != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
            printk("[VIRTIO-GPU] GET_DISPLAY_INFO failed (0x%x)\n",
                   (uint64_t)resp);
            return -1;
        }

        /* With virtio-vga, the VGA compat layer provides the actual
         * framebuffer resolution.  The GPU scanout may report a
         * different default (e.g. 640x480).  Always use the
         * framebuffer dimensions for consistency. */
        disp_width  = fb_get_width();
        disp_height = fb_get_height();

        printk("[VIRTIO-GPU] Display: %ux%u\n",
               (uint64_t)disp_width, (uint64_t)disp_height);
    }

    /* ---- Create framebuffer resource ---- */
    {
        struct virtio_gpu_resource_create_2d cmd;
        gpu_memset(&cmd, 0, sizeof(cmd));
        cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
        cmd.resource_id = RES_FRAMEBUFFER;
        cmd.format      = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
        cmd.width       = disp_width;
        cmd.height      = disp_height;

        resp = gpu_ctrl_send(&cmd, sizeof(cmd));
        if (resp != VIRTIO_GPU_RESP_OK_NODATA) {
            printk("[VIRTIO-GPU] RESOURCE_CREATE_2D (fb) failed (0x%x)\n",
                   (uint64_t)resp);
            return -1;
        }
    }

    /* ---- Attach backing memory (use the framebuffer back buffer) ---- */
    {
        fb_backing = (uint32_t *)fb_get_backbuffer();
        fb_backing_size = disp_width * disp_height * 4;

        gpu_memset(&attach_cmd, 0, sizeof(attach_cmd));
        attach_cmd.cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
        attach_cmd.cmd.resource_id = RES_FRAMEBUFFER;
        attach_cmd.cmd.nr_entries  = 1;
        attach_cmd.entry.addr      = (uint64_t)fb_backing;
        attach_cmd.entry.length    = fb_backing_size;

        resp = gpu_ctrl_send(&attach_cmd,
                             sizeof(struct virtio_gpu_resource_attach_backing) +
                             sizeof(struct virtio_gpu_mem_entry));
        if (resp != VIRTIO_GPU_RESP_OK_NODATA) {
            printk("[VIRTIO-GPU] ATTACH_BACKING failed (0x%x)\n",
                   (uint64_t)resp);
            return -1;
        }
    }

    /* ---- Set scanout ---- */
    {
        struct virtio_gpu_set_scanout cmd;
        gpu_memset(&cmd, 0, sizeof(cmd));
        cmd.hdr.type     = VIRTIO_GPU_CMD_SET_SCANOUT;
        cmd.r.x          = 0;
        cmd.r.y          = 0;
        cmd.r.width      = disp_width;
        cmd.r.height     = disp_height;
        cmd.scanout_id   = 0;
        cmd.resource_id  = RES_FRAMEBUFFER;

        resp = gpu_ctrl_send(&cmd, sizeof(cmd));
        if (resp != VIRTIO_GPU_RESP_OK_NODATA) {
            printk("[VIRTIO-GPU] SET_SCANOUT failed (0x%x)\n",
                   (uint64_t)resp);
            return -1;
        }
    }

    /* ---- Create cursor resource (64×64) ---- */
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
            /* Non-fatal: cursor won't work but display will */
        }
    }

    /* Now safe to register IRQ handler (all polling-based init done) */
    idt_register_handler(PIC1_OFFSET + gpu_irq, virtio_gpu_irq);
    pic_unmask_irq(gpu_irq);

    gpu_active = 1;
    printk("[OK] VirtIO-GPU initialized (%ux%u, hw cursor)\n",
           (uint64_t)disp_width, (uint64_t)disp_height);

    return 0;
}

uint8_t virtio_gpu_available(void)
{
    return gpu_active;
}

void virtio_gpu_flush_rect(uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height)
{
    int resp;

    if (!gpu_active)
        return;

    /* Clamp to display bounds */
    if (x + width > disp_width)   width  = disp_width - x;
    if (y + height > disp_height) height = disp_height - y;

    /* Transfer pixel data from backing to GPU resource */
    {
        struct virtio_gpu_transfer_to_host_2d cmd;
        gpu_memset(&cmd, 0, sizeof(cmd));
        cmd.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
        cmd.r.x         = x;
        cmd.r.y         = y;
        cmd.r.width     = width;
        cmd.r.height    = height;
        cmd.offset      = ((uint64_t)y * disp_width + x) * 4;
        cmd.resource_id = RES_FRAMEBUFFER;

        resp = gpu_ctrl_send(&cmd, sizeof(cmd));
        (void)resp;
    }

    /* Flush the region to display */
    {
        struct virtio_gpu_resource_flush cmd;
        gpu_memset(&cmd, 0, sizeof(cmd));
        cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
        cmd.r.x         = x;
        cmd.r.y         = y;
        cmd.r.width     = width;
        cmd.r.height    = height;
        cmd.resource_id = RES_FRAMEBUFFER;

        resp = gpu_ctrl_send(&cmd, sizeof(cmd));
        (void)resp;
    }
}

void virtio_gpu_flush(void)
{
    virtio_gpu_flush_rect(0, 0, disp_width, disp_height);
}

void virtio_gpu_set_cursor(const uint32_t *pixels,
                           uint32_t width, uint32_t height,
                           uint32_t hot_x, uint32_t hot_y)
{
    if (!gpu_active)
        return;

    /* Upload cursor pixels: attach backing, transfer to host */
    /* Allocate a temporary 64×64 buffer (zero-filled for padding) */
    uint32_t cursor_buf[CURSOR_W * CURSOR_H];
    uint32_t row;

    gpu_memset(cursor_buf, 0, sizeof(cursor_buf));

    /* Copy cursor pixels (may be smaller than 64×64) */
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
    cursor_cmd.hdr.type    = VIRTIO_GPU_CMD_UPDATE_CURSOR;
    cursor_cmd.pos.scanout_id = 0;
    cursor_cmd.pos.x       = 0;
    cursor_cmd.pos.y       = 0;
    cursor_cmd.resource_id = RES_CURSOR;
    cursor_cmd.hot_x       = hot_x;
    cursor_cmd.hot_y       = hot_y;

    /* Cursor queue uses single-descriptor (no response) */
    virtq_add_buf(&cursorq, &cursor_cmd, sizeof(cursor_cmd), 0);
    virtq_kick(&cursorq);
}

void virtio_gpu_move_cursor(uint32_t x, uint32_t y)
{
    if (!gpu_active)
        return;

    /* MOVE_CURSOR is very lightweight — no resource transfer,
     * just tells the GPU to reposition the cursor overlay. */
    cursor_cmd.hdr.type    = VIRTIO_GPU_CMD_MOVE_CURSOR;
    cursor_cmd.pos.scanout_id = 0;
    cursor_cmd.pos.x       = x;
    cursor_cmd.pos.y       = y;

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
