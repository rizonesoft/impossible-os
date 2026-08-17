/* ============================================================================
 * boot_confirm.c -- Console keypress primitive that gives the machine back
 *
 * Contract, mode rationale, and the reason a returning primitive is a
 * different shape from the terminal recovery poll: include/kernel/boot_confirm.h.
 *
 * ARCH: x86-64 -- PS/2 port I/O plus the 8259/IOAPIC keyboard routes.
 *
 * XREF: todo/01-boot-platform/TODO-13-tpm-measured-boot-attestation.md
 *       "Trusted Enrollment Provenance"
 * ============================================================================ */

#include "kernel/boot_confirm.h"
#include "kernel/boot_splash.h"
#include "kernel/boot_timing.h"
#include "kernel/drivers/ioapic.h"
#include "kernel/drivers/pic.h"
#include "kernel/drivers/serial.h"
#include "kernel/drivers/watchdog.h"

/* ---- Port I/O ----------------------------------------------------------- */

#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64
#define PS2_STATUS_OBF  0x01  /* output buffer full */
/* Bit 5 of the 8042 status byte: the pending byte came from the AUXILIARY
 * device (the mouse), not the keyboard. Both share port 0x60, so a poll
 * that checks only OBF will happily decode a mouse movement byte as a
 * scancode -- and movement bytes span the whole 0x00-0xFF range, so byte
 * 0x15 would decode as 'y' and CONFIRM a baseline enrollment with nobody
 * touching the keyboard. The same convention is used by the mouse driver
 * at src/kernel/drivers/mouse.c:238. */
#define PS2_STATUS_AUX  0x20
#define PS2_BREAK_CODE  0x80  /* set on key-release scancodes */

/* The ISA lines the two 8042 devices sit on. IRQ12 matters even though this
 * primitive only ever wants keyboard input: both devices share port 0x60,
 * so a live mouse ISR would consume bytes out from under the poll (and the
 * poll would desynchronize the driver's 3-byte packet state machine by
 * discarding AUX bytes). Taking over the controller means taking over both
 * lines, not just the one whose data is wanted. */
#define PS2_KEYBOARD_ISA_IRQ 1
#define PS2_MOUSE_ISA_IRQ    12

/* Bounded drain of the 8042 output buffer. The buffer is a single byte, so
 * 64 reads is far more than a real controller ever holds; the bound exists
 * so a stuck controller that always reports OBF cannot spin here forever
 * before the deadline-bounded poll below ever runs. */
#define PS2_DRAIN_LIMIT 64

/* How often, in poll iterations, to pet the hardware watchdog. The prompt
 * is already bounded below the WDAT floor, so this is defense in depth for
 * the case where a future caller passes a longer timeout. */
#define CONFIRM_PET_INTERVAL 4096u

/* Backstop iteration count for a wait that cannot use the TSC -- a frozen or
 * backward counter, or the BEST_EFFORT caller on a machine whose TSC was
 * never calibrated.
 *
 * SIZED BY ARITHMETIC, not by feel. Each iteration is one or two port reads
 * (~1us each on real hardware) plus a PAUSE, so ~2us per iteration, and 60
 * million iterations is therefore roughly 120 seconds -- the recovery menu's
 * nominal timeout. The first draft used 2,000,000,000 and its comment called
 * that "on the order of minutes"; the arithmetic says closer to an hour. The
 * loop also pets the watchdog as it goes, so WDAT could not have cut the
 * stall short either: a degraded recovery console would have looked hung well
 * past any operator's patience.
 *
 * COARSE on purpose: faster under emulation, slower on sluggish hardware. It
 * is a liveness bound for a path that has no clock, not a timing mechanism,
 * and it is only ever reached when the real time budget is unavailable. */
#define CONFIRM_SPIN_CEILING 60000000ULL

static inline uint8_t confirm_inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline uint64_t confirm_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ---- PS/2 scancode to ASCII (make codes only, US QWERTY) ----------------
 *
 * Deliberately a small set. This primitive asks yes/no questions and drives
 * the recovery menu; it is not a text input path, and a full keymap here
 * would be a second source of truth against the real keyboard driver. */
static char confirm_scancode_to_ascii(uint8_t sc)
{
    switch (sc) {
        case 0x13: return 'r';
        case 0x2E: return 'c';
        case 0x19: return 'p';
        case 0x15: return 'y';
        case 0x31: return 'n';
        case 0x01: return 0x1B;  /* Escape -- a universal decline */
        default:   return 0;
    }
}

/* ---- Interrupt-route save / restore -------------------------------------
 *
 * Only used in BOOT_CONFIRM_IRQ_TAKE_OVER mode. Everything here is a no-op
 * in LEAVE_ALONE mode, which is what the Phase 1 enrollment caller uses. */

struct confirm_irq_state {
    uint64_t rflags;             /* caller's flags, including IF */
    uint32_t kbd_gsi;            /* resolved keyboard GSI */
    uint32_t mouse_gsi;          /* resolved auxiliary-device GSI */
    int      kbd_ioapic_masked;  /* 1/0, or -1 when not applicable */
    int      kbd_pic_masked;     /* 1/0, or -1 when not applicable */
    int      mouse_ioapic_masked;/* 1/0, or -1 when not applicable */
    int      mouse_pic_masked;   /* 1/0, or -1 when not applicable */
    int      took_over;          /* 1 when begin() actually changed anything */
};

/* Restore ONE line on ONE controller to exactly the state it was in. */
static void confirm_restore_line(int was_masked, int is_ioapic, uint32_t gsi,
                                 uint8_t isa_irq)
{
    if (was_masked < 0)
        return;  /* this controller was not applicable; never touched it */
    if (is_ioapic) {
        if (was_masked) ioapic_mask_irq(gsi);
        else            ioapic_unmask_irq(gsi);
    } else {
        if (was_masked) pic_mask_irq(isa_irq);
        else            pic_unmask_irq(isa_irq);
    }
}

static void confirm_irq_begin(struct confirm_irq_state *st,
                              boot_confirm_irq_mode_t mode)
{
    st->rflags = 0;
    st->kbd_gsi = 0;
    st->mouse_gsi = 0;
    st->kbd_ioapic_masked = -1;
    st->kbd_pic_masked = -1;
    st->mouse_ioapic_masked = -1;
    st->mouse_pic_masked = -1;
    st->took_over = 0;

    if (mode != BOOT_CONFIRM_IRQ_TAKE_OVER)
        return;

    /* Save the caller's interrupt flag BEFORE disabling, so restore puts
     * back what was actually there rather than assuming IF was set. */
    __asm__ volatile ("pushfq; pop %0" : "=r"(st->rflags) :: "memory");
    __asm__ volatile ("cli" ::: "memory");
    st->took_over = 1;

    if (ioapic_available()) {
        /* Mask the ROUTED GSIs, not the raw ISA numbers -- ACPI may carry
         * an interrupt source override remapping either line. */
        st->kbd_gsi = ioapic_isa_to_gsi(PS2_KEYBOARD_ISA_IRQ);
        st->kbd_ioapic_masked = ioapic_irq_masked(st->kbd_gsi);
        ioapic_mask_irq(st->kbd_gsi);

        st->mouse_gsi = ioapic_isa_to_gsi(PS2_MOUSE_ISA_IRQ);
        st->mouse_ioapic_masked = ioapic_irq_masked(st->mouse_gsi);
        ioapic_mask_irq(st->mouse_gsi);
    }
    if (pic_available()) {
        st->kbd_pic_masked = pic_irq_masked(PS2_KEYBOARD_ISA_IRQ);
        pic_mask_irq(PS2_KEYBOARD_ISA_IRQ);

        st->mouse_pic_masked = pic_irq_masked(PS2_MOUSE_ISA_IRQ);
        pic_mask_irq(PS2_MOUSE_ISA_IRQ);
    }
}

static void confirm_irq_end(const struct confirm_irq_state *st)
{
    if (!st->took_over)
        return;

    /* Restore every line to the state it was ACTUALLY in. Unmasking
     * unconditionally would enable a line that was masked before this call
     * ran, delivering an interrupt with no registered handler. */
    confirm_restore_line(st->kbd_pic_masked, 0, 0, PS2_KEYBOARD_ISA_IRQ);
    confirm_restore_line(st->mouse_pic_masked, 0, 0, PS2_MOUSE_ISA_IRQ);
    confirm_restore_line(st->kbd_ioapic_masked, 1, st->kbd_gsi, 0);
    confirm_restore_line(st->mouse_ioapic_masked, 1, st->mouse_gsi, 0);

    /* Restore IF last, so every route is correct before any interrupt can
     * be delivered. IF is bit 9 of RFLAGS. */
    if (st->rflags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

/* ---- Console presence ---------------------------------------------------
 *
 * A machine with no PS/2 controller must be told apart from one whose
 * operator simply did not press a key: for a security caller the first is
 * "cannot ask" and the second is "asked and refused". Reading 0xFF from
 * the status port is the standard absent-controller signature (no device
 * drives the bus, so the read floats high). */
static int confirm_console_present(void)
{
    return confirm_inb(PS2_STATUS_PORT) != 0xFF;
}

static void confirm_drain(void)
{
    uint32_t drain = PS2_DRAIN_LIMIT;
    while ((confirm_inb(PS2_STATUS_PORT) & PS2_STATUS_OBF) && drain--)
        (void)confirm_inb(PS2_DATA_PORT);
}

/* Read one KEYBOARD byte if one is pending.
 *
 * Returns 1 and stores the byte when a keyboard byte was consumed; returns
 * 0 when nothing was pending OR when the pending byte belonged to the
 * auxiliary device. An AUX byte is consumed and DISCARDED rather than left
 * in the buffer: leaving it would block the controller and the keyboard
 * byte behind it would never arrive. */
static int confirm_read_kbd_byte(uint8_t *out_sc)
{
    uint8_t status = confirm_inb(PS2_STATUS_PORT);

    if (!(status & PS2_STATUS_OBF))
        return 0;

    if (status & PS2_STATUS_AUX) {
        (void)confirm_inb(PS2_DATA_PORT);  /* mouse byte: drop it */
        return 0;
    }

    *out_sc = confirm_inb(PS2_DATA_PORT);
    return 1;
}

/* Is `ch` an answer this caller will take? NULL accept means "any decoded
 * key", which is the yes/no prompt's mode. */
static int confirm_key_accepted(char ch, const char *accept)
{
    const char *p;
    if (!accept)
        return 1;
    for (p = accept; *p; p++)
        if (*p == ch)
            return 1;
    return 0;
}

/* Poll for one accepted KEYBOARD key until the budget expires.
 *
 * `accept` NULL means "any key this module decodes", which is what the
 * yes/no prompt needs so that a decline is answered immediately instead of
 * being ignored until the deadline and misreported as an absent operator.
 * A non-NULL set is the menu shape, where an unlisted key is not an answer.
 *
 * `budget_ticks` of 0 disables the time bound and `spin_ceiling` of 0
 * disables the iteration bound. Returns the ASCII character, or 0 on expiry.
 *
 * THE ANSWER LANDS ON KEY RELEASE, not on the press, and that is a security
 * property rather than a UI preference. Draining the buffer proves nothing
 * about whether a key is currently DOWN: PS/2 typematic repeat keeps emitting
 * fresh MAKE codes for a held key, so accepting the first make let a taped
 * or pre-held 'y' authorize an enrollment the operator never answered. A
 * stuck key emits makes forever and never breaks, so requiring the matching
 * BREAK of a key whose MAKE we saw is what makes the keypress evidence of a
 * deliberate act. Typematic repeats simply re-arm `pending` and are harmless.
 *
 * Residual, stated rather than papered over: a key already held when the
 * prompt appears and then RELEASED still answers. That is a human action
 * during the prompt window, and closing it too would demand a full
 * release-then-press cycle, which costs an ordinary operator two presses.
 *
 * Expiry is measured as ELAPSED ticks from a captured start, never as an
 * absolute `rdtsc() >= deadline` comparison. Unsigned subtraction is
 * modulo-safe, so a TSC that wraps still yields a correct elapsed value,
 * whereas an absolute deadline computed by addition can wrap to a value the
 * counter has already passed (waiting zero) or can never reach (waiting
 * forever). "Forever" is the dangerous one here, because this loop pets the
 * watchdog and would therefore suppress the very recovery a hang needs.
 *
 * `spin_ceiling` is the backstop for a TSC that is frozen or steps
 * backwards, where NO elapsed computation can ever expire. It is a coarse
 * "this cannot be a real wait any more" bound, not a timing mechanism. */
static char confirm_poll_key(const char *accept, uint64_t budget_ticks,
                             uint64_t spin_ceiling)
{
    uint64_t start = confirm_rdtsc();
    uint64_t spins = 0;
    uint32_t pet = 0;
    char pending = 0;   /* key whose MAKE we have seen since polling began */

    for (;;) {
        uint8_t sc;
        if (confirm_read_kbd_byte(&sc)) {
            /* Strip the break bit BEFORE decoding: a release carries the same
             * scancode with bit 7 set, and this loop needs both halves. */
            char ch = confirm_scancode_to_ascii((uint8_t)(sc & 0x7F));
            if (ch && confirm_key_accepted(ch, accept)) {
                if (!(sc & PS2_BREAK_CODE)) {
                    pending = ch;   /* MAKE: remember it, do not answer yet */
                } else if (pending == ch) {
                    return ch;      /* BREAK for that key: a complete press */
                }
            }
        }

        if (++pet >= CONFIRM_PET_INTERVAL) {
            pet = 0;
            hw_watchdog_pet();
        }

        /* The two bounds are INDEPENDENT. An earlier version nested the spin
         * ceiling inside `if (budget_ticks)`, which left the best-effort
         * caller (no tick budget, ceiling only) with no bound at all. */
        if (budget_ticks && (confirm_rdtsc() - start) >= budget_ticks)
            return 0;
        if (spin_ceiling && ++spins >= spin_ceiling)
            return 0;  /* budget unavailable, or TSC frozen / non-monotonic */

        __asm__ volatile ("pause");
    }
}

/* Convert a millisecond timeout into a tick BUDGET (a duration, not an
 * absolute deadline -- see confirm_poll_key for why that distinction is
 * load-bearing).
 *
 * Writes the budget to *out (0 meaning "unbounded", produced only for an
 * explicit timeout_ms == 0) and returns 1. Returns 0 when a bounded wait
 * was asked for but cannot be honoured: no calibrated TSC, or a
 * multiplication that would overflow. That is a distinct outcome rather
 * than a budget value, because silently degrading either case into an
 * unbounded wait would hang the boot on a keyboard-less machine. */
static int confirm_budget(uint32_t timeout_ms, uint64_t *out)
{
    uint64_t freq, per_ms;

    /* No unbounded mode. An earlier draft let timeout_ms == 0 mean "wait
     * forever" for terminal callers; no caller ever wanted it, and once the
     * time and iteration bounds became independent it silently became
     * spin-bounded anyway -- a contract that said one thing and did another.
     * A boot-path primitive that can wait forever is a hazard, so 0 is
     * simply not a valid request. */
    if (timeout_ms == 0)
        return 0;

    freq = boot_timing_tsc_freq();
    if (freq == 0)
        return 0;

    per_ms = freq / 1000ULL;
    if (per_ms == 0)
        return 0;  /* sub-kHz "frequency" is not a usable clock */

    /* Checked multiply. A calibration glitch reporting an absurd frequency
     * must not silently wrap the budget to a tiny (or zero) value. */
    if (per_ms > (~0ULL / (uint64_t)timeout_ms))
        return 0;

    *out = per_ms * (uint64_t)timeout_ms;
    return 1;
}

boot_confirm_result_t boot_confirm_wait_keys(const char *keys,
                                             char *out_key,
                                             uint32_t timeout_ms,
                                             boot_confirm_irq_mode_t irq_mode,
                                             boot_confirm_clock_t clock_policy)
{
    struct confirm_irq_state st;
    uint64_t budget;
    char ch;

    /* `keys` NULL is the prompt's "any decoded key" mode; an EMPTY string
     * would accept nothing at all and is a caller bug either way. */
    if (keys && !*keys)
        return BOOT_CONFIRM_UNAVAILABLE;

    /* ARGUMENT validation BEFORE clock policy. These are two different kinds
     * of failure and collapsing them was a real contract violation: a zero
     * timeout is an invalid REQUEST, while an uncalibrated TSC is an
     * environment the BEST_EFFORT caller is willing to tolerate. Checking
     * the budget first meant BEST_EFFORT swallowed the invalid argument too
     * and silently ran the long fallback wait instead of returning
     * UNAVAILABLE as the header promises. */
    if (timeout_ms == 0)
        return BOOT_CONFIRM_UNAVAILABLE;

    if (!confirm_console_present())
        return BOOT_CONFIRM_UNAVAILABLE;

    if (!confirm_budget(timeout_ms, &budget)) {
        /* The wait cannot be timed (uncalibrated TSC, or a budget that would
         * overflow). What to do about THAT is the caller's policy. */
        if (clock_policy != BOOT_CONFIRM_CLOCK_BEST_EFFORT)
            return BOOT_CONFIRM_UNAVAILABLE;
        /* Best effort: fall back to the iteration ceiling alone. Coarse and
         * machine-dependent, which is exactly why a security caller may not
         * have it -- but for the recovery menu an inaccurate bound beats no
         * menu at all. */
        budget = 0;
    }

    confirm_irq_begin(&st, irq_mode);
    confirm_drain();
    /* The iteration ceiling applies ONLY when there is no time budget. An
     * earlier version passed it unconditionally, which (once the two bounds
     * were made independent) truncated CALIBRATED waits too: a 120-second
     * recovery menu could expire after ~60 million iterations regardless of
     * the clock. The ceiling is a substitute for a clock, not a companion to
     * one, and the per-iteration cost it would have to be calibrated against
     * is not knowable here -- the no-key path is a single status read, so
     * any fixed conversion is a guess. */
    ch = confirm_poll_key(keys, budget, budget ? 0ULL : CONFIRM_SPIN_CEILING);
    confirm_irq_end(&st);

    if (ch == 0)
        return BOOT_CONFIRM_TIMEOUT;
    if (out_key)
        *out_key = ch;
    return BOOT_CONFIRM_YES;
}

boot_confirm_result_t boot_confirm_prompt(const char *prompt,
                                          char yes_key,
                                          char no_key,
                                          uint32_t timeout_ms,
                                          boot_confirm_irq_mode_t irq_mode,
                                          boot_confirm_clock_t clock_policy)
{
    char pressed = 0;
    boot_confirm_result_t r;

    (void)no_key;  /* advertised in the prompt text; every non-yes key declines */

    if (prompt) {
        /* SPLASH FIRST, and this ordering is the point. The whole premise of
         * a console confirmation is an operator PHYSICALLY AT THE MACHINE --
         * who is looking at the screen, not at a serial terminal. A first
         * version wrote the question only to serial, so that operator saw
         * nothing, pressed nothing, and was logged as an absent timeout: the
         * feature was unusable for exactly the person it exists for.
         *
         * The splash is live here (boot_splash_start_animation() runs earlier
         * in Phase 1), and boot_splash_status() is the same mechanism the
         * neighbouring init steps use for their own status lines.
         *
         * Serial still gets the question too, because a headless operator on
         * a serial console is the other real case. NOTE: serial_write() spins
         * unbounded on a wedged UART (src/kernel/drivers/serial.c:41,699), a
         * pre-existing property of every klog and splash call on this path
         * rather than anything this function introduces -- which is why the
         * timeout below is documented as bounding the KEY WAIT, not the
         * prompt output. Making boot-path serial output bounded is tracked
         * separately; a local workaround here would not make the boot any
         * more survivable. */
        /* boot_splash_status() is a NO-OP when the splash is off, which is a
         * supported configuration (postbars=diag leaves splash_on clear), so
         * calling it is not the same as the question being SEEN. Ask first. */
        /* text_ready, NOT active: boot_splash_active() reports only that the
         * splash is on, and splash_on is set before the font is initialized
         * (src/kernel/boot_splash.c:196 vs 203-205). A font failure would
         * leave it true while boot_splash_status() renders nothing, which
         * reproduces the invisible-prompt hole one level down. */
        int visible = boot_splash_text_ready();
        if (visible)
            boot_splash_status(prompt);
        serial_write(prompt);
        serial_write("\n");

        /* Fail closed when the question could not be shown locally. A
         * security prompt that polls for a keypress nobody was shown is
         * worse than no prompt: it can be answered by stray input and it
         * reports a decision the operator never made. Serial alone does not
         * count here -- this factor exists to prove someone is AT the
         * machine. */
        if (!visible)
            return BOOT_CONFIRM_UNAVAILABLE;
    }

    /* NULL accept set: take ANY key this module decodes. Passing {yes,no}
     * here was a real defect -- Escape and every other decoded key fell
     * outside the set, so a present operator pressing them was ignored until
     * the deadline and then reported as CONFIRM_TIMEOUT, i.e. logged as
     * absent when they had actively refused. The refusal is fail-closed
     * either way, but the security log said the wrong thing about a human. */
    r = boot_confirm_wait_keys((const char *)0, &pressed, timeout_ms,
                               irq_mode, clock_policy);
    if (r != BOOT_CONFIRM_YES)
        return r;  /* TIMEOUT or UNAVAILABLE pass through unchanged */

    return (pressed == yes_key) ? BOOT_CONFIRM_YES : BOOT_CONFIRM_DECLINED;
}
