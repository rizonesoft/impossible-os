/* ============================================================================
 * heap.h -- Kernel Heap Allocator
 *
 * First-fit free-list allocator with block coalescing.
 * Backs onto the VMM/PMM for page allocation.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Initialize the kernel heap (call after VMM is ready).
 * Returns BOOT_OK on success, BOOT_FATAL on failure. */
#include "kernel/boot_init.h"
boot_result_t heap_init(void);

/* Allocate 'size' bytes of kernel memory. Returns NULL on failure */
void *kmalloc(size_t size);

/* Free a previously allocated block */
void kfree(void *ptr);

/* Resize a previously allocated block. Returns NULL on failure */
void *krealloc(void *ptr, size_t new_size);

/* Get heap statistics */
uint64_t heap_get_total(void);
uint64_t heap_get_used(void);
uint64_t heap_get_free(void);

#ifdef KERNEL_TESTS
/* ---- Test-only kmalloc fault injection (00-infrastructure kernel test
 * harness §1) -----------------------------------------------------------
 *
 * Arm the per-CPU counter so the N-th subsequent kmalloc() on THIS CPU
 * returns NULL instead of touching the real allocator. Used by unit
 * tests to exercise the failure-cleanup paths that cannot otherwise be
 * reached on a healthy heap.
 *
 *   kmalloc_fail_countdown_set(N)   arm -- fail the N-th subsequent call
 *                                   (N=1 => next call fails; N=2 => call
 *                                   after next fails; etc.)
 *   kmalloc_fail_next()             == kmalloc_fail_countdown_set(1)
 *   kmalloc_fail_countdown_clear()  disarm immediately
 *
 * The counter is strictly per-CPU (via smp_this_cpu()). A test running
 * on CPU 0 cannot poison a kmalloc from CPU 1.
 *
 * `kmalloc_fail_injections_triggered()` returns the cumulative count of
 * ALL fault-injected NULL returns since boot (aggregate across all
 * CPUs) so tests can assert "the hook actually fired".
 *
 * The test runner auto-clears the counter after each test suite, so a
 * test that arms the countdown and then fails an assertion cannot
 * leak the arming state into the next suite.
 *
 * Thread-context gate: the hook is silently bypassed when the current
 * IRQL is above PASSIVE_LEVEL. An IRQ or DPC that calls kmalloc on the
 * same CPU between `kmalloc_fail_next()` and the intended code-under-
 * test cannot steal the pending injection. Tests that want to inject
 * into IRQ-context allocators need a separate harness. */
void     kmalloc_fail_countdown_set(uint32_t n);
void     kmalloc_fail_countdown_clear(void);
void     kmalloc_fail_next(void);
uint64_t kmalloc_fail_injections_triggered(void);

/* §6 extensions (task-filter + total-hits cap).
 *
 * task filter: when armed with a non-zero pid, the countdown only
 * decrements for the matching task. Sibling kthreads and foreign tasks
 * on the same CPU skip the hook completely (no countdown consumption),
 * so a test that spawns helper kthreads on the SAME CPU isolates the
 * injection to the intended consumer. Scope is PER-CPU: the countdown
 * state lives in the armed CPU's per_cpu_data, so a task that
 * migrates to a different CPU before calling the allocator does NOT
 * trigger -- the test runner is sequential single-CPU (see §7), so
 * this matches the usage pattern; cross-CPU migration-aware testing
 * would need a future broadcast-to-all-CPUs variant.
 *
 * max-injections cap (with auto-reload): setting max_injections=N and
 * arming the countdown (via _next or _set(1)) causes the next N
 * qualifying calls to return NULL. After each fire, the countdown
 * auto-reloads to 1 while fired_counter < max_injections, then stays
 * at 0 once the cap is reached. Delivers 'fail N of the next M calls'
 * multi-fire from a single arm. max_injections=0 disables the cap
 * (classic single-shot countdown semantics).
 *
 * Calling _task_filter_set(0) or _max_injections_set(0) disables the
 * respective gate. Both gates default to 0 after reset. */
void     kmalloc_fail_task_filter_set(uint32_t task_pid);
void     kmalloc_fail_task_filter_clear(void);
void     kmalloc_fail_max_injections_set(uint32_t max);
void     kmalloc_fail_max_injections_clear(void);
uint32_t kmalloc_fail_fired_counter(void);
#endif /* KERNEL_TESTS */
