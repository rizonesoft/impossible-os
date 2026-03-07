/* ============================================================================
 * mouse.c — PS/2 Mouse driver
 *
 * Handles IRQ 12 (interrupt vector 44 after PIC remap).
 * Initializes the PS/2 auxiliary device, parses 3-byte mouse packets,
 * tracks the global cursor position clamped to screen bounds, and renders
 * a 12×19 arrow cursor sprite on the framebuffer.
 *
 * PS/2 mouse protocol:
 *   Byte 0: [Y-ovf][X-ovf][Y-sign][X-sign][1][MBtn][RBtn][LBtn]
 *   Byte 1: X movement (delta, signed via bit 4 of byte 0)
 *   Byte 2: Y movement (delta, signed via bit 5 of byte 0)
 * ============================================================================ */

#include "mouse.h"
#include "idt.h"
#include "pic.h"
#include "framebuffer.h"
#include "printk.h"

/* ---- Port I/O ---- */

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_CMD_PORT     0x64

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* ---- PS/2 controller helpers ---- */

/* Wait until the PS/2 input buffer is empty (ready to accept commands) */
static void ps2_wait_input(void)
{
    uint32_t timeout = 100000;
    while ((inb(PS2_STATUS_PORT) & 0x02) && --timeout)
        ;
}

/* Wait until the PS/2 output buffer is full (data available to read) */
static void ps2_wait_output(void)
{
    uint32_t timeout = 100000;
    while (!(inb(PS2_STATUS_PORT) & 0x01) && --timeout)
        ;
}

/* Send a command byte to the PS/2 controller (port 0x64) */
static void ps2_send_cmd(uint8_t cmd)
{
    ps2_wait_input();
    outb(PS2_CMD_PORT, cmd);
}

/* Send a byte to the PS/2 mouse via the auxiliary port */
static void mouse_write(uint8_t data)
{
    ps2_wait_input();
    outb(PS2_CMD_PORT, 0xD4);   /* Tell controller: next byte is for mouse */
    ps2_wait_input();
    outb(PS2_DATA_PORT, data);
}

/* Read an acknowledgment byte from the mouse */
static uint8_t mouse_read(void)
{
    ps2_wait_output();
    return inb(PS2_DATA_PORT);
}

/* ---- Cursor sprite (12×19 arrow) ----
 * 0 = transparent, 1 = black (outline), 2 = white (fill) */

#define CURSOR_W 12
#define CURSOR_H 19

static const uint8_t cursor_data[CURSOR_H][CURSOR_W] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,1,1,1,1,1},
    {1,2,2,2,1,2,2,1,0,0,0,0},
    {1,2,2,1,0,1,2,2,1,0,0,0},
    {1,2,1,0,0,1,2,2,1,0,0,0},
    {1,1,0,0,0,0,1,2,2,1,0,0},
    {1,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,0,1,2,1,0,0},
    {0,0,0,0,0,0,0,1,1,0,0,0},
};

/* ---- Mouse state ---- */

static volatile int32_t mouse_x;
static volatile int32_t mouse_y;
static volatile uint8_t mouse_buttons;

/* Packet assembly state (3-byte packets) */
static volatile uint8_t  mouse_cycle;     /* 0, 1, or 2 */
static volatile uint8_t  mouse_packet[3];

/* Saved pixels under the cursor (for restore) */
static uint32_t saved_under[CURSOR_H][CURSOR_W];
static int32_t  saved_x = -1;
static int32_t  saved_y = -1;

/* ---- IRQ 12 handler ---- */

static uint64_t mouse_irq_handler(struct interrupt_frame *frame)
{
    uint8_t status = inb(PS2_STATUS_PORT);

    /* Bit 0 = output buffer full, Bit 5 = mouse data (auxiliary) */
    if (!(status & 0x01)) {
        pic_send_eoi(IRQ_MOUSE);
        return (uint64_t)frame;
    }

    uint8_t data = inb(PS2_DATA_PORT);

    switch (mouse_cycle) {
    case 0:
        /* Byte 0: only accept if bit 3 is set (always-1 in PS/2 protocol) */
        if (data & 0x08) {
            mouse_packet[0] = data;
            mouse_cycle = 1;
        }
        break;

    case 1:
        mouse_packet[1] = data;
        mouse_cycle = 2;
        break;

    case 2:
        mouse_packet[2] = data;
        mouse_cycle = 0;

        {
            /* Decode deltas (signed 9-bit values) */
            int32_t dx = (int32_t)mouse_packet[1];
            int32_t dy = (int32_t)mouse_packet[2];

            /* Sign-extend using bits 4 and 5 of byte 0 */
            if (mouse_packet[0] & 0x10) dx |= (int32_t)0xFFFFFF00;
            if (mouse_packet[0] & 0x20) dy |= (int32_t)0xFFFFFF00;

            /* Discard if overflow bits are set */
            if (mouse_packet[0] & 0xC0)
                break;

            /* PS/2 Y-axis is inverted (positive = up) */
            mouse_x += dx;
            mouse_y -= dy;

            /* Clamp to screen bounds */
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_y < 0) mouse_y = 0;
            if ((uint32_t)mouse_x >= fb_get_width())
                mouse_x = (int32_t)fb_get_width() - 1;
            if ((uint32_t)mouse_y >= fb_get_height())
                mouse_y = (int32_t)fb_get_height() - 1;

            /* Update button state */
            mouse_buttons = mouse_packet[0] & 0x07;
        }
        break;
    }

    pic_send_eoi(IRQ_MOUSE);
    return (uint64_t)frame;
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

void mouse_init(void)
{
    uint8_t status_byte;

    /* Start cursor at screen center */
    mouse_x = (int32_t)(fb_get_width() / 2);
    mouse_y = (int32_t)(fb_get_height() / 2);
    mouse_buttons = 0;
    mouse_cycle = 0;

    /* Step 1: Enable the auxiliary (mouse) device on the PS/2 controller */
    ps2_send_cmd(0xA8);

    /* Step 2: Enable IRQ 12 in the PS/2 controller's config byte */
    ps2_send_cmd(0x20);        /* Read config byte */
    ps2_wait_output();
    status_byte = inb(PS2_DATA_PORT);
    status_byte |= 0x02;       /* Bit 1 = enable IRQ 12 (auxiliary) */
    status_byte &= ~0x20;      /* Bit 5 = 0 = enable mouse clock */
    ps2_send_cmd(0x60);        /* Write config byte */
    ps2_wait_input();
    outb(PS2_DATA_PORT, status_byte);

    /* Step 3: Use default settings */
    mouse_write(0xF6);         /* Set defaults */
    mouse_read();              /* ACK */

    /* Step 4: Set sample rate to 100 samples/sec */
    mouse_write(0xF3);         /* Set sample rate command */
    mouse_read();              /* ACK */
    mouse_write(100);          /* 100 samples/sec */
    mouse_read();              /* ACK */

    /* Step 5: Set resolution to 4 counts/mm */
    mouse_write(0xE8);         /* Set resolution command */
    mouse_read();              /* ACK */
    mouse_write(0x02);         /* 4 counts/mm */
    mouse_read();              /* ACK */

    /* Step 6: Enable data reporting */
    mouse_write(0xF4);         /* Enable */
    mouse_read();              /* ACK */

    /* Step 7: Flush any pending data */
    while (inb(PS2_STATUS_PORT) & 0x01)
        inb(PS2_DATA_PORT);

    /* Step 8: Register IRQ 12 handler (vector 44 = 32 + 12) */
    idt_register_handler(44, mouse_irq_handler);

    /* Step 9: Unmask IRQ 12 on the PIC */
    pic_unmask_irq(IRQ_MOUSE);

    printk("[OK] PS/2 mouse initialized (IRQ 12)\n");
}

/* ============================================================================
 * State query
 * ============================================================================ */

struct mouse_state mouse_get_state(void)
{
    struct mouse_state s;
    s.x = mouse_x;
    s.y = mouse_y;
    s.buttons = mouse_buttons;
    return s;
}

/* ============================================================================
 * Cursor rendering
 * ============================================================================ */

void mouse_save_under(void)
{
    /* Not implemented yet — requires reading from back buffer.
     * For now, the WM will handle save/restore via dirty rects. */
    saved_x = mouse_x;
    saved_y = mouse_y;
    (void)saved_under;
}

void mouse_restore_under(void)
{
    /* Placeholder — WM dirty rect system will handle cursor restore */
    (void)saved_x;
    (void)saved_y;
}

void mouse_draw_cursor(void)
{
    int32_t cx = mouse_x;
    int32_t cy = mouse_y;
    uint32_t px, py;
    uint32_t scr_w = fb_get_width();
    uint32_t scr_h = fb_get_height();

    for (py = 0; py < CURSOR_H; py++) {
        for (px = 0; px < CURSOR_W; px++) {
            uint8_t pix = cursor_data[py][px];
            if (pix == 0)
                continue;  /* transparent */

            uint32_t sx = (uint32_t)(cx + (int32_t)px);
            uint32_t sy = (uint32_t)(cy + (int32_t)py);

            if (sx >= scr_w || sy >= scr_h)
                continue;

            uint32_t color = (pix == 1) ? 0x00000000 : 0x00FFFFFF;
            fb_put_pixel(sx, sy, color);
        }
    }
}
