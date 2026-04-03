/* ============================================================================
 * mouse.c -- PS/2 Mouse driver
 *
 * Handles IRQ 12 (interrupt vector 44 after PIC remap).
 * Initializes the PS/2 auxiliary device, parses 3-byte mouse packets,
 * tracks the global cursor position clamped to screen bounds.
 *
 * PS/2 mouse protocol:
 *   Byte 0: [Y-ovf][X-ovf][Y-sign][X-sign][1][MBtn][RBtn][LBtn]
 *   Byte 1: X movement (delta, signed via bit 4 of byte 0)
 *   Byte 2: Y movement (delta, signed via bit 5 of byte 0)
 * ============================================================================ */

#include "kernel/drivers/mouse.h"
#include "kernel/irq.h"
#include "kernel/drivers/pic.h"
#include "kernel/drivers/ioapic.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/acpi.h"
#include "kernel/boot_init.h"
#include "kernel/klog.h"
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
    uint32_t timeout = 1000000;
    while (--timeout) {
        uint8_t s = inb(PS2_STATUS_PORT);
        if (s == 0xFF) break;       /* controller absent */
        if (!(s & 0x02)) break;     /* input buffer empty */
    }
}

static void ps2_wait_output(void)
{
    uint32_t timeout = 1000000;
    while (--timeout) {
        uint8_t s = inb(PS2_STATUS_PORT);
        if (s == 0xFF) break;       /* controller absent */
        if (s & 0x01) break;        /* output buffer full */
    }
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

/* ---- Mouse state ---- */

static volatile int32_t mouse_x;
static volatile int32_t mouse_y;
static volatile uint8_t mouse_buttons;

static volatile uint8_t  mouse_cycle;
static volatile uint8_t  mouse_packet[3];

/* Debug counter -- how many IRQ 12 interrupts we've received */
static volatile uint32_t mouse_irq_count;

/* ---- IRQ 12 handler (via irq_register API) ---- */

static void mouse_irq_callback(uint8_t vector, void *ctx)
{
    uint8_t status = inb(PS2_STATUS_PORT);

    (void)vector;
    (void)ctx;

    mouse_irq_count++;
    /* Bit 0 = output buffer full, Bit 5 = mouse data */
    if (!(status & 0x01))
        return;  /* EOI handled by wrapper */

    uint8_t data = inb(PS2_DATA_PORT);

    /* Only process if bit 5 indicates auxiliary (mouse) data */
    if (!(status & 0x20))
        return;  /* EOI handled by wrapper */

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

            /* Clamp deltas to reject unreasonable jumps. */
            #define MOUSE_DELTA_MAX 127
            if (dx > MOUSE_DELTA_MAX)  dx = MOUSE_DELTA_MAX;
            if (dx < -MOUSE_DELTA_MAX) dx = -MOUSE_DELTA_MAX;
            if (dy > MOUSE_DELTA_MAX)  dy = MOUSE_DELTA_MAX;
            if (dy < -MOUSE_DELTA_MAX) dy = -MOUSE_DELTA_MAX;

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
    /* EOI handled by irq_dispatch_wrapper */
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

void mouse_init(void)
{
    uint8_t status_byte;

    POST16(0xD503);

    /* Gate: skip if no i8042 controller present.
     * Probe port 0x64 directly -- FADT is unreliable on QEMU WHPX. */
    if (!acpi_has_8042()) {
        uint8_t probe = inb(0x64);
        if (probe == 0xFF) {
            klog(LOG_INFO, "input", "PS/2 mouse: skipped (no i8042 -- port 0x64 reads 0xFF)");
            return;
        }
        klog(LOG_INFO, "input", "PS/2 mouse: FADT says no i8042 but port probe OK (0x%x)",
             (uint32_t)probe);
    }

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

    /* If status reads 0xFF, the PS/2 controller has no auxiliary port
     * (common on laptops with USB/I2C touchpads).  Bail out. */
    if (status_byte == 0xFF) {
        klog(LOG_INFO, "input", "PS/2 mouse: no auxiliary port (touchpad/USB?)");
        return;
    }
    status_byte |= 0x02;       /* Bit 1 = enable IRQ 12 */
    status_byte &= ~0x20;      /* Bit 5 = 0 = enable mouse clock */
    ps2_send_cmd(0x60);
    ps2_wait_input();
    outb(PS2_DATA_PORT, status_byte);

    /* Probe: reset mouse and check for ACK (0xFA).  If the auxiliary
     * port has no device (laptop touchpad via USB/I2C), the read
     * returns garbage after timeout -- skip full init. */
    mouse_write(0xFF);  /* reset */
    {
        uint8_t ack = mouse_read();
        if (ack != 0xFA) {
            klog(LOG_INFO, "input",
                 "PS/2 mouse: no ACK on reset (0x%x) -- skipping",
                 (uint64_t)ack);
            return;
        }
        mouse_read();  /* self-test result (0xAA) */
        mouse_read();  /* device ID (0x00) */
    }

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

    /* Flush -- timeout prevents hang on platforms without i8042 */
    {
        uint32_t timeout = 1024;
        while ((inb(PS2_STATUS_PORT) & 0x01) && --timeout)
            inb(PS2_DATA_PORT);
    }

    /* Register mouse via GSI-based routing (IOAPIC handles vector allocation) */
    if (ioapic_available()) {
        irq_request_gsi(ioapic_isa_to_gsi(IRQ_MOUSE),
                         mouse_irq_callback, (void *)0, "ps2_mouse");
    } else {
        irq_register(44, mouse_irq_callback, (void *)0, "ps2_mouse");
        pic_unmask_irq(IRQ_MOUSE);
    }

    POST16(0xD504);
    klog(LOG_DEBUG, "input", "PS/2 mouse initialized (IRQ 12)");
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

uint32_t mouse_get_irq_count(void)
{
    return mouse_irq_count;
}

void mouse_set_position(int32_t x, int32_t y)
{
    mouse_x = x;
    mouse_y = y;
}

void mouse_inject_state(int32_t x, int32_t y, uint8_t buttons)
{
    mouse_x = x;
    mouse_y = y;
    mouse_buttons = buttons;

    /* Clamp to screen bounds */
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if ((uint32_t)mouse_x >= fb_get_width())
        mouse_x = (int32_t)fb_get_width() - 1;
    if ((uint32_t)mouse_y >= fb_get_height())
        mouse_y = (int32_t)fb_get_height() - 1;
}

