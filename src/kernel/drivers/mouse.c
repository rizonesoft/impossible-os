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
#include "kernel/vectors.h"
#include "kernel/drivers/pic.h"
#include "kernel/drivers/ioapic.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/acpi.h"
#include "kernel/boot_init.h"
#include "kernel/klog.h"
#include "kernel/boot_timing.h"
#include "kernel/sched/spinlock.h"

/* TSC sample helper for per-step profiling. */
static inline uint64_t mouse_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

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

/* Two timeout budgets:
 *  - PS2_WAIT_SHORT (~10ms on trapped I/O) for input/output buffer state
 *    polls.  These complete in microseconds on healthy controllers; a
 *    100ms+ wait here means the controller is wedged.
 *  - PS2_WAIT_LONG  (~500ms) for the BAT/reset response read where real
 *    PS/2 hardware can legitimately take 300ms-2s to respond.
 *
 * s_timeout_hits counts iterations where the loop ran to exhaustion --
 * exposed for the post-init profiling summary so a wedged controller is
 * attributable per-call rather than just visible as wall-clock cost. */
/* Real PS/2 hardware: BAT/self-test can take 300ms-2s.  Sized to cover
 * the documented worst case so a slow-but-valid device is not skipped. */
#define PS2_WAIT_SHORT    10000u
#define PS2_WAIT_LONG  2000000u

static uint32_t s_timeout_hits;

static int ps2_wait_input(void)
{
    uint32_t timeout = PS2_WAIT_SHORT;
    while (--timeout) {
        uint8_t s = inb(PS2_STATUS_PORT);
        if (s == 0xFF) return 0;       /* controller absent */
        if (!(s & 0x02)) return 1;     /* input buffer empty */
    }
    s_timeout_hits++;
    return 0;
}

static int ps2_wait_output(void)
{
    uint32_t timeout = PS2_WAIT_SHORT;
    while (--timeout) {
        uint8_t s = inb(PS2_STATUS_PORT);
        if (s == 0xFF) return 0;       /* controller absent */
        if (s & 0x01) return 1;        /* output buffer full */
    }
    s_timeout_hits++;
    return 0;
}

/* Long-deadline output wait for BAT/reset responses.  Returns 1 when
 * data arrives within PS2_WAIT_LONG iterations; 0 on timeout. */
static int ps2_wait_output_long(void)
{
    uint32_t timeout = PS2_WAIT_LONG;
    while (--timeout) {
        uint8_t s = inb(PS2_STATUS_PORT);
        if (s == 0xFF) return 0;
        if (s & 0x01) return 1;
    }
    s_timeout_hits++;
    return 0;
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

/* Read + validate that the device returned 0xFA (ACK).  Returns 1 on
 * ACK, 0 on timeout/NACK so the caller can abort init rather than
 * proceed with a desynchronized command stream. */
static int mouse_expect_ack(void)
{
    if (!ps2_wait_output()) return 0;
    return inb(PS2_DATA_PORT) == 0xFA;
}

/* Long-deadline read for BAT/reset response sequences.  Returns 1 on
 * success (data byte stored in *out), 0 on timeout.  Real PS/2 hardware
 * can take 300ms-2s for self-test; the normal short-deadline
 * mouse_read() would skip valid devices, and the prior void-return
 * shape silently masked timeouts as "successful read of garbage". */
static int mouse_read_long(uint8_t *out)
{
    if (!ps2_wait_output_long()) {
        if (out) *out = 0xFF;
        return 0;
    }
    if (out) *out = inb(PS2_DATA_PORT);
    else (void)inb(PS2_DATA_PORT);
    return 1;
}

/* ---- Mouse state ---- */

static volatile int32_t mouse_x;
static volatile int32_t mouse_y;
static volatile uint8_t mouse_buttons;   /* published = OR of all sources */

/* Per-source button state. The published mouse_buttons is the OR of these so a
 * movement report from one device (buttons=0) cannot release a button still
 * held on another (e.g. dragging with a USB mouse while a VirtIO tablet polls).
 * All written + OR-published under mouse_lock. */
static volatile uint8_t s_ps2_buttons;
static volatile uint8_t s_usb_buttons;
static volatile uint8_t s_abs_buttons;   /* VirtIO/VBox/Hyper-V absolute source */

/* Last absolute report seen by mouse_merge_absolute -- edge detection so an
 * unchanged absolute source does not overwrite relative (USB/PS2) deltas every
 * frame. abs_valid=0 until the first absolute report. Guarded by mouse_lock. */
static volatile int32_t s_last_abs_x;
static volatile int32_t s_last_abs_y;
static volatile uint8_t s_abs_valid;

/* Set once the cursor position has been published by any input source. The USB
 * HID poller (armed during xhci_init, which runs before deferred PS/2 init) can
 * publish via mouse_update_relative before mouse_init runs; the seed there is
 * conditional on this flag so deferred PS/2 init does not re-center a cursor a
 * USB mouse already moved. Guarded by mouse_lock. */
static volatile uint8_t mouse_seeded;

/* Guards the compound read-modify-clamp-write of mouse_x/mouse_y/mouse_buttons.
 * Writers: PS/2 IRQ12 ISR, the USB HID tick-ISR poller (mouse_update_relative),
 * the compositor thread (mouse_set_position). Readers: mouse_get_state (every
 * compositor frame, IF=1). volatile alone keeps the three fields from being
 * elided but does NOT make the RMW atomic or give a consistent x/y/buttons
 * snapshot across an ISR boundary -- irqsave so an interrupt-context writer
 * cannot deadlock against a thread-context holder on the same CPU. */
static spinlock_t mouse_lock = SPINLOCK_INIT;

/* Clamp mouse_x/mouse_y to the framebuffer. Caller MUST hold mouse_lock. */
static void mouse_clamp_locked(void)
{
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if ((uint32_t)mouse_x >= fb_get_width())
        mouse_x = (int32_t)fb_get_width() - 1;
    if ((uint32_t)mouse_y >= fb_get_height())
        mouse_y = (int32_t)fb_get_height() - 1;
}

/* Publish mouse_buttons as the union of all source button states. Caller MUST
 * hold mouse_lock. */
static void mouse_publish_buttons_locked(void)
{
    mouse_buttons = (uint8_t)(s_ps2_buttons | s_usb_buttons | s_abs_buttons);
}

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

            {
                uint64_t flags;
                spin_lock_irqsave(&mouse_lock, &flags);
                /* PS/2 Y-axis is inverted */
                mouse_x += dx;
                mouse_y -= dy;
                mouse_clamp_locked();
                s_ps2_buttons = mouse_packet[0] & 0x07;
                mouse_publish_buttons_locked();
                spin_unlock_irqrestore(&mouse_lock, flags);
            }
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
    const char *exit_reason = "ok";
    uint64_t t_start = mouse_rdtsc();
    s_timeout_hits = 0;

    POST16(0xD503);

    /* Seed the cursor at screen center FIRST -- before the i8042 presence
     * checks below, which early-exit on no-i8042 / ACPI-hardware-reduced
     * systems. Seeding here means a USB-only machine still shows a centered
     * cursor at idle. Only seed if no source has published yet (a USB mouse can
     * publish via mouse_update_relative before this deferred init runs); guard
     * under mouse_lock so the check + stores cannot race the tick-ISR updater. */
    {
        int32_t cx = (int32_t)(fb_get_width() / 2);
        int32_t cy = (int32_t)(fb_get_height() / 2);
        uint64_t flags;
        spin_lock_irqsave(&mouse_lock, &flags);
        if (!mouse_seeded) {
            mouse_x = cx;
            mouse_y = cy;
            mouse_buttons = 0;
            mouse_seeded = 1;
        }
        spin_unlock_irqrestore(&mouse_lock, flags);
    }

    /* Gate: never touch 0x60/0x64 unless an i8042 is actually present.
     * Hardware-reduced ACPI hard-skips; otherwise probe port 0x64 directly
     * (FADT IAPC_BOOT_ARCH.8042 is unreliable on QEMU WHPX). The probe runs
     * unconditionally so a no/short-FADT box with no i8042 is also skipped. */
    if (acpi_hw_reduced()) {
        exit_reason = "ACPI hardware-reduced (no i8042)";
        goto report;
    }
    {
        uint8_t probe = inb(0x64);
        if (probe == 0xFF) {
            exit_reason = "no i8042 (port 0x64 reads 0xFF)";
            goto report;
        }
        if (!acpi_has_8042())
            klog(LOG_INFO, "input", "PS/2 mouse: FADT says no i8042 but port probe OK (0x%x)",
                 (uint32_t)probe);
    }

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
        exit_reason = "no auxiliary port (touchpad/USB?)";
        goto report;
    }
    /* Enable the mouse clock now, but keep IRQ12 (bit 1) DISABLED until the
     * device is confirmed working below. Otherwise an aborted init (NACK/BAT
     * fail / timeout) leaves IRQ12 unmasked on a controller with no working
     * mouse, producing spurious IRQ12s. */
    status_byte &= ~0x20;      /* Bit 5 = 0 = enable mouse clock */
    status_byte &= ~0x02;      /* Bit 1 = 0 = IRQ 12 stays off for now */
    ps2_send_cmd(0x60);
    ps2_wait_input();
    outb(PS2_DATA_PORT, status_byte);

    /* Probe: reset mouse and check for ACK (0xFA).
     * - ACK is immediate from the controller (microseconds): short
     *   deadline. A no-mouse port times out after ~10ms, not 2s.
     * - The subsequent self-test (BAT) + device-ID reads can take
     *   300ms-2s on real hardware: long deadline. BAT must be 0xAA
     *   before proceeding -- a wrong/missing BAT means the bus state
     *   is desynchronized and follow-on commands cannot be trusted. */
    mouse_write(0xFF);  /* reset */
    {
        uint8_t ack = mouse_read();
        if (ack != 0xFA) {
            klog(LOG_INFO, "input",
                 "PS/2 mouse: no ACK on reset (0x%x) -- skipping",
                 (uint64_t)ack);
            exit_reason = "reset NACK";
            goto report;
        }
        uint8_t bat = 0;
        if (!mouse_read_long(&bat) || bat != 0xAA) {
            klog(LOG_WARN, "input",
                 "PS/2 mouse: BAT failed (got 0x%x, expected 0xAA) -- skipping",
                 (uint64_t)bat);
            exit_reason = "BAT fail";
            goto report;
        }
        uint8_t dev_id = 0;
        if (!mouse_read_long(&dev_id)) {
            klog(LOG_WARN, "input",
                 "PS/2 mouse: device-ID read timed out -- skipping");
            exit_reason = "device-ID timeout";
            goto report;
        }
    }

    /* Set defaults (F6) */
    mouse_write(0xF6);
    if (!mouse_expect_ack()) { exit_reason = "F6 NACK"; goto report; }

    /* Set sample rate to 100 (F3 + 100) */
    mouse_write(0xF3);
    if (!mouse_expect_ack()) { exit_reason = "F3 NACK"; goto report; }
    mouse_write(100);
    if (!mouse_expect_ack()) { exit_reason = "F3 data NACK"; goto report; }

    /* Set resolution to 4 counts/mm (E8 + 0x02) */
    mouse_write(0xE8);
    if (!mouse_expect_ack()) { exit_reason = "E8 NACK"; goto report; }
    mouse_write(0x02);
    if (!mouse_expect_ack()) { exit_reason = "E8 data NACK"; goto report; }

    /* Enable data reporting (F4) */
    mouse_write(0xF4);
    if (!mouse_expect_ack()) { exit_reason = "F4 NACK"; goto report; }

    /* Flush -- timeout prevents hang on platforms without i8042 */
    {
        uint32_t timeout = 1024;
        while ((inb(PS2_STATUS_PORT) & 0x01) && --timeout)
            inb(PS2_DATA_PORT);
    }

    /* Device confirmed working (reset/BAT/defaults/sample/res/F4 all ACKed) --
     * NOW enable IRQ 12, so an earlier abort never left it on. Set bit 1 on the
     * CACHED config byte (clock-on, IRQ12-off) we wrote above and write it
     * straight back. Do NOT re-read the config: data reporting is enabled, so
     * the output buffer may hold a mouse data packet, not the config byte -- a
     * read here would treat packet data as config and corrupt the controller. */
    status_byte |= 0x02;       /* Bit 1 = enable IRQ 12 */
    ps2_send_cmd(0x60);
    ps2_wait_input();
    outb(PS2_DATA_PORT, status_byte);

    /* Register mouse via GSI-based routing (IOAPIC handles vector allocation) */
    if (ioapic_available()) {
        irq_request_gsi(ioapic_isa_to_gsi(IRQ_MOUSE),
                         mouse_irq_callback, (void *)0, "ps2_mouse");
    } else {
        irq_register(VECTOR_PS2_MOUSE, mouse_irq_callback, (void *)0, "ps2_mouse");
        pic_unmask_irq(IRQ_MOUSE);
    }

    POST16(0xD504);
    klog(LOG_INFO, "input", "PS/2 mouse initialized (IRQ %u, 100 samples/sec, 4 counts/mm)",
         ioapic_available() ? (uint32_t)ioapic_isa_to_gsi(IRQ_MOUSE)
                            : (uint32_t)IRQ_MOUSE);

report:
    /* Per-init profile + skip-reason summary.  Funneled here so every
     * exit path -- success and the three early-return cases -- emits
     * the same TSC/timeout-hit data and a structured reason string. */
    {
        uint64_t elapsed_ms = boot_timing_tsc_delta_ms(mouse_rdtsc() - t_start);
        klog(LOG_INFO, "input",
             "PS/2 mouse: init %ums, timeouts=%u, exit=%s",
             elapsed_ms, (uint64_t)s_timeout_hits, exit_reason);
    }
}

/* ============================================================================
 * State query
 * ============================================================================ */

struct mouse_state mouse_get_state(void)
{
    struct mouse_state s;
    uint64_t flags;
    spin_lock_irqsave(&mouse_lock, &flags);
    s.x = mouse_x;
    s.y = mouse_y;
    s.buttons = mouse_buttons;
    spin_unlock_irqrestore(&mouse_lock, flags);
    return s;
}

uint32_t mouse_get_irq_count(void)
{
    return mouse_irq_count;
}

void mouse_set_position(int32_t x, int32_t y)
{
    uint64_t flags;
    spin_lock_irqsave(&mouse_lock, &flags);
    mouse_x = x;
    mouse_y = y;
    spin_unlock_irqrestore(&mouse_lock, flags);
}

void mouse_inject_state(int32_t x, int32_t y, uint8_t buttons)
{
    uint64_t flags;
    spin_lock_irqsave(&mouse_lock, &flags);
    mouse_x = x;
    mouse_y = y;
    s_abs_buttons = buttons;   /* Hyper-V synthetic mouse is an absolute source */
    mouse_publish_buttons_locked();
    mouse_clamp_locked();
    mouse_seeded = 1;   /* published: deferred PS/2 init must not re-center */
    spin_unlock_irqrestore(&mouse_lock, flags);
}

/* Edge-triggered absolute position merge. Caller MUST hold mouse_lock. Only an
 * absolute report whose coordinates CHANGED (or the first report) moves the
 * cursor, so between absolute updates a relative mouse's deltas persist. */
static void mouse_merge_pos_locked(int32_t x, int32_t y)
{
    if (!s_abs_valid || x != s_last_abs_x || y != s_last_abs_y) {
        mouse_x = x;
        mouse_y = y;
        mouse_clamp_locked();
        mouse_seeded = 1;
        s_last_abs_x = x;
        s_last_abs_y = y;
        s_abs_valid = 1;
    }
}

/* Merge a button-bearing absolute pointing source (VirtIO tablet) into the
 * shared cursor. Position is edge-triggered (see mouse_merge_pos_locked).
 * Absolute buttons are tracked in their own source slot and OR-published, so a
 * button-only or idle absolute report never releases a button held on a
 * relative device. Compositor-thread use. */
void mouse_merge_absolute(int32_t x, int32_t y, uint8_t buttons)
{
    uint64_t flags;
    spin_lock_irqsave(&mouse_lock, &flags);
    mouse_merge_pos_locked(x, y);
    s_abs_buttons = buttons;
    mouse_publish_buttons_locked();
    spin_unlock_irqrestore(&mouse_lock, flags);
}

/* Merge a position-ONLY absolute source (VBox VMMDev) into the shared cursor.
 * VBox provides absolute position but NOT buttons -- its button state is read
 * back from the PS/2 mouse, so feeding it into the absolute button slot would
 * echo (and then latch) a relative device's held button. Buttons therefore
 * flow only through the PS/2 source slot here. Compositor-thread use. */
void mouse_merge_absolute_position(int32_t x, int32_t y)
{
    uint64_t flags;
    spin_lock_irqsave(&mouse_lock, &flags);
    mouse_merge_pos_locked(x, y);
    spin_unlock_irqrestore(&mouse_lock, flags);
}

void mouse_update_relative(int32_t dx, int32_t dy, uint8_t buttons)
{
    int32_t cx = (int32_t)(fb_get_width() / 2);
    int32_t cy = (int32_t)(fb_get_height() / 2);
    uint64_t flags;
    spin_lock_irqsave(&mouse_lock, &flags);
    /* If a USB report arrives before deferred PS/2 init seeds the cursor, base
     * this relative delta on screen center -- not the (0,0) origin -- and mark
     * seeded so mouse_init keeps this position instead of re-centering. */
    if (!mouse_seeded) {
        mouse_x = cx;
        mouse_y = cy;
        mouse_seeded = 1;
    }
    /* USB HID boot mouse reports +Y downward (screen-oriented), so apply dy
     * directly -- unlike the PS/2 wire byte which is +Y up (mouse_y -= dy). */
    mouse_x += dx;
    mouse_y += dy;
    mouse_clamp_locked();
    s_usb_buttons = buttons;
    mouse_publish_buttons_locked();
    spin_unlock_irqrestore(&mouse_lock, flags);
}

#ifdef KERNEL_TESTS
void mouse_test_set_ps2_buttons(uint8_t buttons)
{
    uint64_t flags;
    spin_lock_irqsave(&mouse_lock, &flags);
    s_ps2_buttons = buttons;
    mouse_publish_buttons_locked();
    spin_unlock_irqrestore(&mouse_lock, flags);
}
#endif

