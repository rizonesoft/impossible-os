/* ============================================================================
 * boot_confirm.h -- Console keypress primitive that GIVES THE MACHINE BACK
 *
 * boot_recovery.c has polled PS/2 scancodes since the recovery screen
 * shipped, but its lifecycle is terminal by construction: kbd_poll_begin()
 * executed `cli` and masked the keyboard GSI with no restore counterpart
 * anywhere in the tree, which was correct only because every caller ran
 * straight into reboot / power-off / halt and never had to keep booting.
 *
 * A confirmation prompt is the opposite shape. It must return and the boot
 * must continue on ALL outcomes -- confirmed, declined, timed out -- so a
 * refused prompt costs a baseline enrollment and never the machine.
 *
 * TWO MODES, and picking the wrong one is the bug this header exists to
 * prevent:
 *
 *   BOOT_CONFIRM_IRQ_LEAVE_ALONE -- for callers running BEFORE the
 *     keyboard driver has claimed IRQ1. IOAPIC redirection entries start
 *     masked (src/kernel/drivers/ioapic.c:295) and keyboard_init() requests
 *     the GSI only at src/kernel/drivers/keyboard.c:335, so at Phase 1
 *     enrollment time nothing else is reading port 0x60 and there is
 *     nothing to mask. Leaving interrupts ENABLED here is not a shortcut:
 *     it keeps the timer alive so the splash spinner keeps animating and
 *     the hardware watchdog keeps being petted through the prompt.
 *
 *   BOOT_CONFIRM_IRQ_TAKE_OVER -- for callers running AFTER the keyboard
 *     driver is live, which would otherwise race the driver for scancodes.
 *     Saves the interrupt flag and the current routes for BOTH 8042 lines
 *     (keyboard IRQ1 and auxiliary IRQ12, on the IOAPIC and the PIC alike),
 *     masks them, and restores each to what it actually was on every exit
 *     path. IRQ12 is included because both devices share port 0x60 and a
 *     live mouse ISR would otherwise eat bytes the poll is waiting for.
 *
 *     LIMIT, stated because the obvious reading of "restores everything" is
 *     wrong. Masking IRQ12 stops the mouse DRIVER from running; it does not
 *     stop the DEVICE from streaming, and this poll discards the auxiliary
 *     bytes it sees. The driver's 3-byte packet cursor is therefore not
 *     guaranteed to be where it was when IRQ12 comes back. Today's only
 *     TAKE_OVER caller is the recovery screen, which is TERMINAL -- every
 *     action it returns ends in reboot / power-off / halt -- so nothing
 *     ever observes the desync. A future caller that takes over after
 *     mouse_init() and then KEEPS BOOTING must first quiesce the device and
 *     resynchronize the driver's parser; that work is tracked in the owning
 *     TODO and is a prerequisite for such a caller, not an optional extra.
 *
 * The prompt is always bounded by a tick BUDGET derived from the calibrated
 * TSC and compared as elapsed time from a captured start, never by a spin
 * count and never by an absolute deadline: a spin count is a different
 * duration on every machine, and an absolute deadline built by addition can
 * wrap to a value the counter never reaches. That matters because the poll
 * pets the watchdog, so an unbounded wait would suppress the very recovery
 * a hang needs -- and this runs while an ACPI WDAT watchdog is already
 * armed with a 60-second floor (src/kernel/drivers/watchdog.c:30-33).
 *
 * ARCH: x86-64 -- PS/2 port I/O and the 8259/IOAPIC routes are
 * x86-specific; this file moves under arch/ with the rest of the boot
 * console when the ARM64 port lands.
 *
 * XREF: todo/01-boot-platform/TODO-13-tpm-measured-boot-attestation.md
 *       "Trusted Enrollment Provenance"
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* How the primitive should treat the keyboard interrupt route. */
typedef enum {
    /* Nothing else owns IRQ1 yet: do not touch interrupt state at all. */
    BOOT_CONFIRM_IRQ_LEAVE_ALONE = 0,
    /* The keyboard driver is live: save, mask, and restore both 8042 routes. */
    BOOT_CONFIRM_IRQ_TAKE_OVER   = 1,
} boot_confirm_irq_mode_t;

/* What to do when the TSC is not calibrated (boot_timing_tsc_freq() == 0).
 *
 * This is a POLICY, not a detail, because the two callers need opposite
 * answers and hardcoding either one breaks the other. An uncalibrated TSC is
 * a supported state, not a fault: boot_timing_init() logs "TSC frequency
 * unknown" and continues (src/kernel/boot_timing.c:247-250).
 *
 * The distinction was learned the hard way -- a first version applied the
 * security policy to both callers, which made the degraded-boot recovery
 * menu halt instantly on exactly the machines that most need it. */
typedef enum {
    /* SECURITY callers. No trustworthy clock means no bounded prompt, so
     * report the console unusable and let the caller fail closed. Guessing a
     * duration is not acceptable when the answer authorizes a write. */
    BOOT_CONFIRM_CLOCK_REQUIRED    = 0,
    /* RECOVERY callers. A coarse iteration-bounded wait is far better than
     * no menu at all; the bound only has to guarantee the machine cannot sit
     * here forever, not to be accurate. */
    BOOT_CONFIRM_CLOCK_BEST_EFFORT = 1,
} boot_confirm_clock_t;

/* What the operator did. Mirrors tpm_confirm_outcome_t's meanings without
 * depending on it, so the primitive stays reusable by callers that have
 * nothing to do with the TPM. */
typedef enum {
    BOOT_CONFIRM_YES         = 0,  /* the affirmative key was pressed */
    BOOT_CONFIRM_DECLINED    = 1,  /* the decline key, or any other key */
    BOOT_CONFIRM_TIMEOUT     = 2,  /* deadline expired with no keypress */
    BOOT_CONFIRM_UNAVAILABLE = 3,  /* no usable PS/2 console to prompt on */
} boot_confirm_result_t;

/* Longest a boot may sit on a confirmation prompt. Well under the WDAT
 * 60-second floor so a prompt can never be what expires the watchdog, and
 * long enough that an operator who is standing there can read the line and
 * decide. */
#define BOOT_CONFIRM_TIMEOUT_MS 15000u

/* Ask the operator a yes/no question on the PS/2 console and RETURN.
 *
 * `prompt` is written to serial (and is never NULL-dereferenced). `yes_key`
 * is lowercase ASCII. EVERY other recognized key -- `no_key`, Escape, or any
 * other make code this module decodes -- returns BOOT_CONFIRM_DECLINED, so a
 * present operator who does not want this gets an immediate, correctly
 * attributed answer instead of waiting out the timeout and being logged as
 * absent. `no_key` therefore only documents which key the prompt advertises;
 * it does not narrow what counts as a decline.
 *
 * The question is rendered on the boot splash AND written to serial, because
 * the operator this exists for is standing at the machine; a serial-only
 * prompt is invisible to them.
 *
 * `timeout_ms` bounds THE KEY WAIT, not the prompt output. That distinction
 * is honest rather than pedantic: serial_write() spins unbounded on a wedged
 * UART (src/kernel/drivers/serial.c:41), a property shared by every klog and
 * splash call on the boot path, so no local bound here could make the claim
 * whole. Pass BOOT_CONFIRM_TIMEOUT_MS unless you have a reason not to, and
 * never a value at or above the watchdog floor. A timeout_ms of 0 is invalid
 * and returns BOOT_CONFIRM_UNAVAILABLE.
 *
 * Returns BOOT_CONFIRM_UNAVAILABLE when no PS/2 controller answers, and
 * (under BOOT_CONFIRM_CLOCK_REQUIRED) when the wait cannot be bounded. Both
 * are REFUSALS for a security caller, not a pass. Interrupt state and the
 * 8042 routes are always as the caller left them on return. */
boot_confirm_result_t boot_confirm_prompt(const char *prompt,
                                          char yes_key,
                                          char no_key,
                                          uint32_t timeout_ms,
                                          boot_confirm_irq_mode_t irq_mode,
                                          boot_confirm_clock_t clock_policy);

/* Wait for one of a SET of keys, the shape the recovery menu needs.
 *
 * `keys` is a NUL-terminated set of lowercase ASCII keys to accept; unlike
 * the prompt above, keys outside the set are ignored rather than treated as
 * a decline, because a menu has no "no" answer. On a keypress the matching
 * character is stored in *out_key and the call returns BOOT_CONFIRM_YES; on
 * expiry it returns BOOT_CONFIRM_TIMEOUT with *out_key untouched.
 *
 * `timeout_ms` must be non-zero: there is deliberately NO unbounded mode,
 * because a boot-path primitive that can wait forever is a hazard and no
 * caller wanted one. A 0 returns BOOT_CONFIRM_UNAVAILABLE.
 *
 * Same restore guarantee as boot_confirm_prompt(). */
boot_confirm_result_t boot_confirm_wait_keys(const char *keys,
                                             char *out_key,
                                             uint32_t timeout_ms,
                                             boot_confirm_irq_mode_t irq_mode,
                                             boot_confirm_clock_t clock_policy);
