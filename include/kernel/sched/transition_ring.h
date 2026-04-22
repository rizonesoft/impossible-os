/* ============================================================================
 * transition_ring.h -- Per-CPU fast-path transition ring buffer (fast-path transition ring)
 *
 * Every ring-0 <-> ring-3 boundary crossing on a CPU gets recorded into that
 * CPU's lock-free ring. The last 64 transitions are dumped on panic before
 * the CR2/register block so a silent user-mode hang still leaves a
 * replayable trace showing exactly which transition left the CPU in the
 * state that triggered the crash.
 *
 * Record sites (all three native transports covered):
 *   - ISR entry when coming from ring 3  (INT 0x80, INT 0x2E, hardware IRQs)
 *   - ISR exit returning to ring 3       (iretq path in isr_handler tail)
 *   - SYSCALL entry via syscall_dispatch_fast from syscall_entry.asm
 *
 * Why always-on instead of opt-in perf/ftrace: Windows xperf and Linux
 * perf both require external configuration before a user-mode hang.
 * When the hang actually happens, the tracing isn't running. §19's ring
 * is baked into every panic, so the data is there the first time it
 * matters -- no second repro needed.
 *
 * Lock-free by construction: only the owning CPU writes its ring; other
 * CPUs read only via the panic dump path, which is quiesced by the
 * existing `s_panic_owner` CAS guard (panic.c:257-287).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

struct per_cpu_data;

/* Initialize the ring on the current CPU. Must be called after `gs:0`
 * self-pointer is valid (i.e. after `smp_early_bsp_init` on the BSP and
 * after AP startup writes GS_BASE on APs). Idempotent: repeat calls are
 * no-ops after the init marker is set. */
void transition_ring_init_this_cpu(void);

/* Record a ring-3 transition on the current CPU. Safe to call from:
 *   - ISR entry (C side of isr_common_stub, i.e. isr_handler)
 *   - ISR exit (same, tail)
 *   - SYSCALL dispatch fast-path (syscall_dispatch_fast's C body)
 * Does not take any lock. Passes `direction` as TRANSITION_DIR_TO_KERNEL
 * on ring-3 -> ring-0 entry, TRANSITION_DIR_TO_USER on ring-0 -> ring-3
 * exit. `rip` and `rsp` are the USER-mode values at the boundary
 * (caller's responsibility to pick them out of the iret frame or the
 * per-CPU user-RSP scratch for SYSCALL). Other state (tsc, cr3, gs
 * bases) is sampled inside this function. */
void transition_ring_record(uint32_t direction, uint64_t rip, uint64_t rsp);

/* Dump the ring for the given CPU to serial, oldest-first. Called from
 * panic.c while the s_panic_owner guard is held. Safe to call even if
 * the ring was never initialized (emits `RING: not-initialized` in
 * that case). */
void transition_ring_dump_to_serial(struct per_cpu_data *pcpu);
