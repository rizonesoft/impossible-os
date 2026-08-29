/* ============================================================================
 * stack_canary.h -- -fstack-protector-strong kernel cookie
 *
 * The kernel is built with -fstack-protector-strong -mstack-protector-guard=global,
 * so every function with stack buffers / address-taken locals reads the global
 * __stack_chk_guard cookie on entry and compares it on return; a smash routes
 * to __stack_chk_fail -> BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE. NT-style single
 * global cookie (not per-task).
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* The compiler-referenced global cookie (defined once in stack_canary.c). */
extern uintptr_t __stack_chk_guard;

/* Seed __stack_chk_guard from RDRAND (TSC + kernel-base fallback). Call ONCE,
 * early, from a non-returning context (kernel_main, after Phase-0 CPUID) so no
 * live stack-protected frame spans the guard write. */
void canary_init(void);

/* Pure cookie massaging (unit-testable): low byte forced to 0 (terminator
 * canary), bit 63 set so the cookie is never 0 and the high byte is non-zero. */
uintptr_t canary_massage(uint64_t raw);

/* Pure predicate (unit-testable): is a RANDOM_SEED boot-payload descriptor safe
 * to peek for canary entropy? Requires, IN THIS ORDER: the negotiated
 * BOOT_CAP_PAYLOAD_DESCRIPTORS capability (without it the PMM reservation pass
 * never ran, so FLAG_RESERVED certifies nothing and the range may already be
 * allocator-owned), FLAG_RESERVED itself, length >= 16, and
 * [phys_start, phys_start+length) wholly inside the 4 GiB boot identity map
 * (canary_init runs pre-IDT, so an out-of-map read would #PF-hang). */
int canary_seed_desc_ok(uint64_t caps_present, uint32_t flags,
                        uint64_t phys_start, uint64_t length);
