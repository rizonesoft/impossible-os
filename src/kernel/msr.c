/* ============================================================================
 * msr.c -- #GP-safe MSR probe implementation
 *
 * msr_try_read() temporarily installs a #GP handler that sets a flag
 * instead of panicking, allowing safe detection of unsupported MSRs.
 *
 * XREF: 02-kernel-core/TODO-19-x86-64-architecture.md §3
 * ============================================================================ */

#include "kernel/msr.h"
#include "kernel/idt.h"

/* Flag set by the temporary #GP handler */
static volatile int s_gp_fired;

/* Saved original #GP handler */
static interrupt_handler_t s_saved_gp_handler;

/* Temporary #GP handler: set flag, skip the faulting rdmsr (2 bytes) */
static uint64_t gp_probe_handler(struct interrupt_frame *frame)
{
    s_gp_fired = 1;
    /* rdmsr is a 2-byte instruction (0F 32); skip it */
    frame->rip += 2;
    return (uint64_t)frame;
}

int msr_try_read(uint32_t index, uint64_t *out)
{
    uint64_t val;

    /* Save current #GP handler and install our probe */
    s_saved_gp_handler = idt_get_handler(13);
    s_gp_fired = 0;
    idt_register_handler(13, gp_probe_handler);

    /* Attempt the read -- if the MSR doesn't exist, #GP fires
     * and gp_probe_handler skips the instruction + sets flag */
    val = msr_read(index);

    /* Restore original handler */
    idt_register_handler(13, s_saved_gp_handler);

    if (s_gp_fired) {
        if (out) *out = 0;
        return -1;
    }

    if (out) *out = val;
    return 0;
}
