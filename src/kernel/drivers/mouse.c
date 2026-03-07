/* ============================================================================
 * mouse.c — PS/2 Mouse driver
 *
 * Handles IRQ 12 (interrupt vector 44 after PIC remap).
 * Initializes the PS/2 auxiliary device, parses 3-byte mouse packets,
 * tracks the global cursor position clamped to screen bounds, and renders
 * a 12×19 arrow cursor sprite on the framebuffer with save/restore.
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

static void ps2_wait_input(void)
{
    uint32_t timeout = 100000;
    while ((inb(PS2_STATUS_PORT) & 0x02) && --timeout)
        ;
}

static void ps2_wait_output(void)
{
    uint32_t timeout = 100000;
    while (!(inb(PS2_STATUS_PORT) & 0x01) && --timeout)
        ;
}

static void ps2_send_cmd(uint8_t cmd)
{
    ps2_wait_input();
    outb(PS2_CMD_PORT, cmd);
}

static void mouse_write(uint8_t data)
{
    ps2_wait_input();
    outb(PS2_CMD_PORT, 0xD4);   /* next byte goes to mouse */
    ps2_wait_input();
    outb(PS2_DATA_PORT, data);
}

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

/* Packet assembly */
static volatile uint8_t  mouse_cycle;
static volatile uint8_t  mouse_packet[3];

/* Saved pixels under the cursor for restore */
static uint32_t saved_under[CURSOR_H][CURSOR_W];
static int32_t  saved_x = -1;
static int32_t  saved_y = -1;
static uint8_t  cursor_visible = 0;

/* ---- IRQ 12 handler ---- */

static uint64_t mouse_irq_handler(struct interrupt_frame *frame)
{
    uint8_t status = inb(PS2_STATUS_PORT);

    /* Bit 0 = output buffer full, Bit 5 = mouse data */
    if (!(status & 0x01)) {
        pic_send_eoi(IRQ_MOUSE);
        return (uint64_t)frame;
    }

    uint8_t data = inb(PS2_DATA_PORT);

    /* Only process if bit 5 indicates auxiliary (mouse) data */
    if (!(status & 0x20)) {
        pic_send_eoi(IRQ_MOUSE);
        return (uint64_t)frame;
    }

    switch (mouse_cycle) {
    case 0:
        /* Byte 0: only accept if bit 3 is set (always-1 in PS/2) */
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
            int32_t dx = (int32_t)mouse_packet[1];
            int32_t dy = (int32_t)mouse_packet[2];

            /* Sign-extend using bits 4 and 5 of byte 0 */
            if (mouse_packet[0] & 0x10) dx |= (int32_t)0xFFFFFF00;
            if (mouse_packet[0] & 0x20) dy |= (int32_t)0xFFFFFF00;

            /* Discard if overflow */
            if (mouse_packet[0] & 0xC0)
                break;

            /* PS/2 Y-axis is inverted */
            mouse_x += dx;
            mouse_y -= dy;

            /* Clamp to screen bounds */
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_y < 0) mouse_y = 0;
            if ((uint32_t)mouse_x >= fb_get_width())
                mouse_x = (int32_t)fb_get_width() - 1;
            if ((uint32_t)mouse_y >= fb_get_height())
                mouse_y = (int32_t)fb_get_height() - 1;

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

    /* Enable auxiliary (mouse) device */
    ps2_send_cmd(0xA8);

    /* Enable IRQ 12 in PS/2 config byte */
    ps2_send_cmd(0x20);
    ps2_wait_output();
    status_byte = inb(PS2_DATA_PORT);
    status_byte |= 0x02;       /* Bit 1 = enable IRQ 12 */
    status_byte &= ~0x20;      /* Bit 5 = 0 = enable mouse clock */
    ps2_send_cmd(0x60);
    ps2_wait_input();
    outb(PS2_DATA_PORT, status_byte);

    /* Set defaults */
    mouse_write(0xF6);
    mouse_read();

    /* Set sample rate to 100 */
    mouse_write(0xF3);
    mouse_read();
    mouse_write(100);
    mouse_read();

    /* Set resolution to 4 counts/mm */
    mouse_write(0xE8);
    mouse_read();
    mouse_write(0x02);
    mouse_read();

    /* Enable data reporting */
    mouse_write(0xF4);
    mouse_read();

    /* Flush */
    while (inb(PS2_STATUS_PORT) & 0x01)
        inb(PS2_DATA_PORT);

    /* Register IRQ 12 (vector 44) */
    idt_register_handler(44, mouse_irq_handler);
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
 * Cursor rendering with save/restore
 * ============================================================================ */

void mouse_save_under(void)
{
    /* Already saved or no cursor to save under */
    (void)0;
}

void mouse_restore_under(void)
{
    uint32_t px, py;
    uint32_t scr_w, scr_h;

    if (!cursor_visible)
        return;

    scr_w = fb_get_width();
    scr_h = fb_get_height();

    /* Restore saved pixels at the old cursor position */
    for (py = 0; py < CURSOR_H; py++) {
        for (px = 0; px < CURSOR_W; px++) {
            if (cursor_data[py][px] == 0)
                continue;  /* was transparent, nothing to restore */

            uint32_t sx = (uint32_t)(saved_x + (int32_t)px);
            uint32_t sy = (uint32_t)(saved_y + (int32_t)py);

            if (sx < scr_w && sy < scr_h)
                fb_put_pixel(sx, sy, saved_under[py][px]);
        }
    }

    cursor_visible = 0;
}

void mouse_draw_cursor(void)
{
    int32_t cx = mouse_x;
    int32_t cy = mouse_y;
    uint32_t px, py;
    uint32_t scr_w = fb_get_width();
    uint32_t scr_h = fb_get_height();

    /* First, restore the old cursor position */
    mouse_restore_under();

    /* Save the pixels under the new cursor position */
    saved_x = cx;
    saved_y = cy;

    for (py = 0; py < CURSOR_H; py++) {
        for (px = 0; px < CURSOR_W; px++) {
            uint32_t sx = (uint32_t)(cx + (int32_t)px);
            uint32_t sy = (uint32_t)(cy + (int32_t)py);

            if (sx < scr_w && sy < scr_h)
                saved_under[py][px] = fb_read_pixel(sx, sy);
            else
                saved_under[py][px] = 0;
        }
    }

    /* Draw the cursor sprite */
    for (py = 0; py < CURSOR_H; py++) {
        for (px = 0; px < CURSOR_W; px++) {
            uint8_t pix = cursor_data[py][px];
            if (pix == 0)
                continue;

            uint32_t sx = (uint32_t)(cx + (int32_t)px);
            uint32_t sy = (uint32_t)(cy + (int32_t)py);

            if (sx >= scr_w || sy >= scr_h)
                continue;

            uint32_t color = (pix == 1) ? 0x00000000 : 0x00FFFFFF;
            fb_put_pixel(sx, sy, color);
        }
    }

    cursor_visible = 1;
}
