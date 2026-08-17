/* ============================================================================
 * boot_recovery.c -- Degraded-boot recovery screen
 *
 * In-kernel graphical recovery UI shown when a Phase 2/3 subsystem fails
 * non-fatally. Renders directly to the GOP framebuffer using fb_fill_rect
 * and an inline 8x8 bitmap font. No heap allocation, no compositor.
 *
 * Displays: which subsystem failed, POST code, phase number.
 * Menu: [R] Retry, [C] Serial console, [P] Power off.
 * Keypress polling goes through boot_confirm.c (the one PS/2 poll
 * lifecycle in the tree); this screen is a terminal caller and uses its
 * IRQ take-over mode.
 *
 * XREF: 02-kernel-core/TODO-01-kernel-init-sequencing.md
 * ============================================================================ */

#include "kernel/boot_recovery.h"
#include "kernel/boot_confirm.h"
#include "kernel/boot_init.h"
#include "kernel/boot_halt.h"
#include "kernel/boot_info.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/drivers/serial.h"
#include "kernel/klog.h"
#include "kernel/uefi_runtime.h"

/* ---- Keypress polling ---------------------------------------------------
 *
 * The PS/2 poll lifecycle lives in boot_confirm.c so there is exactly ONE
 * of it. This screen is a TERMINAL caller -- every action it returns ends
 * in reboot / power-off / halt -- so it takes the IRQ TAKE_OVER mode and
 * does not care that interrupts are off while it waits. The primitive
 * still restores them on the way out, which costs nothing here and is what
 * makes the same code reusable by a caller that must keep booting.
 *
 * The old spin-count backstop is gone: a spin budget is a different
 * duration on every machine, and the primitive bounds the wait by the
 * calibrated TSC instead. */

/* Wall-clock backstop so a headless / keyboard-less machine does not sit on
 * the recovery menu forever. Generous, because a human reading a recovery
 * screen and deciding is exactly the case this must not cut short.
 *
 * Paired with BOOT_CONFIRM_CLOCK_BEST_EFFORT, and that pairing is the point:
 * this screen exists precisely because the boot went wrong, so it must still
 * work on a machine whose TSC never got calibrated. The security caller uses
 * the opposite policy -- it would rather refuse than guess a duration. */
#define RECOVERY_KBD_TIMEOUT_MS 120000u

/* ---- Inline 8x8 bitmap font (minimal subset for recovery text) ---------- */

#define FONT_W 8
#define FONT_H 10  /* 8px glyph + 2px line spacing */

static const uint8_t s_font[128][8] = {
    /* Only printable ASCII 0x20-0x7E. Everything else = solid block. */
    [0x00 ... 0x1F] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
    [0x20] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},  /* space */
    [0x21] = {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},  /* ! */
    [0x28] = {0x08,0x10,0x10,0x10,0x10,0x10,0x08,0x00},  /* ( */
    [0x29] = {0x20,0x10,0x10,0x10,0x10,0x10,0x20,0x00},  /* ) */
    [0x2D] = {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},  /* - */
    [0x2E] = {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},  /* . */
    [0x30] = {0x3C,0x66,0x6E,0x7E,0x76,0x66,0x3C,0x00},  /* 0 */
    [0x31] = {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},  /* 1 */
    [0x32] = {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00},  /* 2 */
    [0x33] = {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00},  /* 3 */
    [0x34] = {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00},  /* 4 */
    [0x35] = {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00},  /* 5 */
    [0x36] = {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00},  /* 6 */
    [0x37] = {0x7E,0x06,0x0C,0x18,0x18,0x18,0x18,0x00},  /* 7 */
    [0x38] = {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00},  /* 8 */
    [0x39] = {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00},  /* 9 */
    [0x3A] = {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00},  /* : */
    [0x41] = {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00},  /* A */
    [0x42] = {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00},  /* B */
    [0x43] = {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00},  /* C */
    [0x44] = {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00},  /* D */
    [0x45] = {0x7E,0x60,0x60,0x78,0x60,0x60,0x7E,0x00},  /* E */
    [0x46] = {0x7E,0x60,0x60,0x78,0x60,0x60,0x60,0x00},  /* F */
    [0x48] = {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00},  /* H */
    [0x49] = {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},  /* I */
    [0x4C] = {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00},  /* L */
    [0x4D] = {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00},  /* M */
    [0x4E] = {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00},  /* N */
    [0x4F] = {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},  /* O */
    [0x50] = {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00},  /* P */
    [0x52] = {0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00},  /* R */
    [0x53] = {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00},  /* S */
    [0x54] = {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00},  /* T */
    [0x56] = {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00},  /* V */
    [0x58] = {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00},  /* X */
    [0x5B] = {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},  /* [ */
    [0x5D] = {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},  /* ] */
    [0x61] = {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00},  /* a */
    [0x62] = {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00},  /* b */
    [0x63] = {0x00,0x00,0x3C,0x60,0x60,0x60,0x3C,0x00},  /* c */
    [0x64] = {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00},  /* d */
    [0x65] = {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00},  /* e */
    [0x66] = {0x0E,0x18,0x3E,0x18,0x18,0x18,0x18,0x00},  /* f */
    [0x68] = {0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0x00},  /* h */
    [0x69] = {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},  /* i */
    [0x6C] = {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},  /* l */
    [0x6D] = {0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00},  /* m */
    [0x6E] = {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00},  /* n */
    [0x6F] = {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00},  /* o */
    [0x70] = {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60},  /* p */
    [0x72] = {0x00,0x00,0x6C,0x76,0x60,0x60,0x60,0x00},  /* r */
    [0x73] = {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00},  /* s */
    [0x74] = {0x18,0x18,0x7E,0x18,0x18,0x18,0x0E,0x00},  /* t */
    [0x75] = {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00},  /* u */
    [0x76] = {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00},  /* v */
    [0x77] = {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},  /* w */
    [0x78] = {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00},  /* x */
    [0x79] = {0x00,0x00,0x66,0x66,0x3E,0x06,0x3C,0x00},  /* y */
    [0x7F] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},  /* DEL = block */
};

/* ---- Direct framebuffer text rendering ---------------------------------- */

static void draw_char(volatile uint32_t *fb, uint32_t pitch,
                      uint32_t scr_w, uint32_t scr_h,
                      uint32_t x, uint32_t y, char c, uint32_t color)
{
    uint8_t idx = (uint8_t)c;
    if (idx >= 128) idx = 0x7F;
    const uint8_t *glyph = s_font[idx];
    uint32_t row, col;
    for (row = 0; row < 8; row++) {
        if (y + row >= scr_h) break;
        uint8_t bits = glyph[row];
        for (col = 0; col < 8; col++) {
            if (x + col >= scr_w) break;
            if (bits & (0x80 >> col))
                fb[(y + row) * pitch + (x + col)] = color;
        }
    }
}

static void draw_string(volatile uint32_t *fb, uint32_t pitch,
                        uint32_t scr_w, uint32_t scr_h,
                        uint32_t x, uint32_t y, const char *s,
                        uint32_t color)
{
    while (*s) {
        draw_char(fb, pitch, scr_w, scr_h, x, y, *s, color);
        x += FONT_W;
        s++;
    }
}

static void draw_hex16(volatile uint32_t *fb, uint32_t pitch,
                       uint32_t scr_w, uint32_t scr_h,
                       uint32_t x, uint32_t y, uint16_t val, uint32_t color)
{
    static const char hex[] = "0123456789ABCDEF";
    char buf[7] = { '0', 'x', hex[(val >> 12) & 0xF], hex[(val >> 8) & 0xF],
                    hex[(val >> 4) & 0xF], hex[val & 0xF], '\0' };
    draw_string(fb, pitch, scr_w, scr_h, x, y, buf, color);
}

/* ---- Subsystem name lookup ----------------------------------------------
 *
 * Single source of truth: kernel_subsystem_name() in boot_init.c.
 * Previously this file maintained a duplicate names[] table that drifted
 * out of sync when SUBSYS_COUNT grew (caught by Codex 2026-04-08 during
 * the Phase 0 propagation fix -- C zero-fills missing initializers,
 * so the static_assert on size was satisfied while the new slots quietly
 * returned NULL pointers that would crash draw_string() at recovery
 * time). The duplicate table is gone; both call sites now read from one
 * canonical table in boot_init.c. */

/* ---- Recovery screen ---------------------------------------------------- */

boot_recovery_action_t boot_recovery_show(const boot_recovery_info_t *info)
{
    volatile uint32_t *fb;
    uint32_t w, h, pitch;
    uint32_t y;

    boot_progress(9, "recovery-screen", 0xE0);

    /* Serial output always works */
    serial_write("\n[RECOVERY] Boot failure in Phase ");
    serial_putchar('0' + (info->phase & 0x0F));
    serial_write(": ");
    serial_write(kernel_subsystem_name(info->subsystem));
    serial_write("\n");

    /* If framebuffer is not ready, fall through to boot_halt() */
    if (!kernel_subsystem_ready(SUBSYS_FB)) {
        boot_halt("Recovery screen unavailable (no framebuffer)");
        /* boot_halt never returns, but satisfy the compiler */
        for (;;) __asm__ volatile ("hlt");
    }

    /* Unlock the compositor and render into the driver's BACK buffer using its
     * stride, then fb_swap() presents it to the visible VRAM page. Drawing
     * straight to g_boot_info.fb.addr would be overwritten by the stale back
     * buffer whenever a separate back buffer is allocated (fb_swap copies
     * back_buf -> VRAM); when no back buffer exists, fb_get_backbuffer() == the
     * hardware address and fb_swap() is a no-op, so this is correct either way. */
    fb_unlock_compositor();
    w     = fb_get_width();
    h     = fb_get_height();
    pitch = fb_get_stride();                          /* back-buffer pixels/row */
    fb    = (volatile uint32_t *)fb_get_backbuffer();

    if (!fb || !w || !h) {
        boot_halt("Recovery screen: invalid framebuffer");
        for (;;) __asm__ volatile ("hlt");
    }

    /* Dark blue background */
    {
        uint32_t row, col;
        for (row = 0; row < h; row++)
            for (col = 0; col < w; col++)
                fb[row * pitch + col] = 0x00001A33;
    }

    /* Title bar */
    y = 40;
    draw_string(fb, pitch, w, h, 40, y, "BOOT RECOVERY", 0x00FF6060);
    y += FONT_H * 2;

    /* Failure info */
    draw_string(fb, pitch, w, h, 40, y, "A subsystem failed during boot.", 0x00FFFFFF);
    y += FONT_H * 2;

    draw_string(fb, pitch, w, h, 40, y, "Phase:     ", 0x00C0C0C0);
    draw_char(fb, pitch, w, h, 40 + FONT_W * 11, y, '0' + (info->phase & 0x0F), 0x00FFFFFF);
    y += FONT_H;

    draw_string(fb, pitch, w, h, 40, y, "Subsystem: ", 0x00C0C0C0);
    draw_string(fb, pitch, w, h, 40 + FONT_W * 11, y, kernel_subsystem_name(info->subsystem), 0x00FF8080);
    y += FONT_H;

    draw_string(fb, pitch, w, h, 40, y, "POST code: ", 0x00C0C0C0);
    draw_hex16(fb, pitch, w, h, 40 + FONT_W * 11, y, info->postcode, 0x00FFFF80);
    y += FONT_H;

    draw_string(fb, pitch, w, h, 40, y, "Result:    ", 0x00C0C0C0);
    draw_string(fb, pitch, w, h, 40 + FONT_W * 11, y,
                info->result == BOOT_FATAL ? "FATAL" : "DEGRADED", 0x00FF8080);
    y += FONT_H * 3;

    /* Menu */
    draw_string(fb, pitch, w, h, 40, y, "Choose an action:", 0x00FFFFFF);
    y += FONT_H * 2;

    draw_string(fb, pitch, w, h, 60, y, "[R]  Retry boot", 0x0080FF80);
    y += FONT_H + 4;
    draw_string(fb, pitch, w, h, 60, y, "[C]  Halt to serial log", 0x0080CCFF);
    y += FONT_H + 4;
    draw_string(fb, pitch, w, h, 60, y, "[P]  Power off", 0x00FF8080);

    /* Swap to front buffer */
    fb_swap();

    /* Poll for the user's choice, bounded so a headless / keyboard-less
     * machine cannot wait here forever. On expiry (or with no usable PS/2
     * console at all) halt with a serial banner so an operator or hardware
     * watchdog can power-cycle -- the failure was already printed to serial
     * above. The primitive drains stale scancodes and takes over the
     * keyboard route itself. */
    {
        char ch = 0;
        boot_confirm_result_t r =
            boot_confirm_wait_keys("rcp", &ch, RECOVERY_KBD_TIMEOUT_MS,
                                   BOOT_CONFIRM_IRQ_TAKE_OVER,
                                   BOOT_CONFIRM_CLOCK_BEST_EFFORT);
        if (r == BOOT_CONFIRM_YES) {
            switch (ch) {
                case 'r': return RECOVERY_RETRY;
                case 'c': return RECOVERY_CONSOLE;
                case 'p': return RECOVERY_POWEROFF;
                default:  break;  /* not in the accept set; unreachable */
            }
        }
        serial_write(r == BOOT_CONFIRM_UNAVAILABLE
                         ? "[RECOVERY] no usable console -- halting "
                           "(power-cycle to retry)\n"
                         : "[RECOVERY] no keyboard input -- halting "
                           "(power-cycle to retry)\n");
        boot_halt("Recovery: no input");
        for (;;) __asm__ volatile ("hlt");
    }
}

/* Centralized recovery-action dispatch so all call sites behave identically
 * (the per-caller `if (act == RECOVERY_POWEROFF)` pattern previously left
 * [R]etry and [C]onsole as no-ops that silently fell through to boot_halt). */
void boot_recovery_act(boot_recovery_action_t act)
{
    extern void acpi_poweroff_now(void);
    extern void acpi_reset_now(void);
    /* Use the quiesce-free ACPI primitives. acpi_shutdown()/acpi_reboot()
     * run acpi_storage_quiesce() first, which sleeps via hlt and hangs
     * forever if interrupts are disabled.
     *
     * The poll primitive now RESTORES the caller's interrupt state on exit,
     * so this no longer runs under a guaranteed cli -- but it is not
     * guaranteed to run with IF set either, because the restore puts back
     * whatever the failing boot happened to have. Quiesce-free is the only
     * choice that is correct under both, which is why it stays. Making the
     * recovery power-off actually flush storage first is a real improvement
     * and is tracked as its own item, not smuggled in here. */
    switch (act) {
        case RECOVERY_POWEROFF:
            acpi_poweroff_now();   /* does not return */
            break;
        case RECOVERY_RETRY:
            /* "Retry boot" = reset the machine; the bootloader A/B tries
             * counter bounds reset loops on a persistently-failing slot. */
            acpi_reset_now();      /* does not return */
            break;
        case RECOVERY_CONSOLE:
            /* The degraded-boot serial console is not implemented yet; tell
             * the operator via serial and return so the caller halts. */
            serial_write("[RECOVERY] serial console not available -- halting\n");
            break;
    }
    /* RECOVERY_CONSOLE (or an unhandled value): return; caller falls to halt. */
}
