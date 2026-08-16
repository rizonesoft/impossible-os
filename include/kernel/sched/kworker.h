/* ============================================================================
 * kworker.h -- system worker thread pool for long-period periodic callbacks
 *
 * A generic "background monitor" primitive: register a callback with a period
 * and the system runs it on a shared kernel thread at that cadence. For SLOW,
 * LONG-period (1+ second) work that may block / page / call firmware -- the
 * opposite of a threaded DPC (TODO-07 threaded DPCs are sub-millisecond bottom
 * halves that must not block). Mirrors NT IoQueueWorkItem / ExpWorkerThread and
 * Linux delayed_work, but with a SINGLE serial worker (see the limitation note
 * on kworker_register), not a parallel pool.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Periodic callback. Runs at PASSIVE_LEVEL on the shared kworker thread. */
typedef void (*kworker_callback_t)(void *ctx);

#define KWORKER_MAX_ENTRIES      16u      /* static slot table size              */
#define KWORKER_MAX_SLEEP_MS     1000u    /* cap idle sleep so a freshly         */
                                          /* registered short period is picked   */
                                          /* up within ~1s                       */
#define KWORKER_CALLBACK_WARN_MS 500u     /* warn if one callback runs longer    */
                                          /* (it delays every other entry)       */

/* Register a periodic callback. Returns a token >= 0, or negative on failure
 * (NULL fn, period_ms == 0, or no free slot).
 *
 * LIMITATION: a SINGLE shared worker thread runs all registered callbacks
 * SERIALLY. A slow or hung callback delays every other entry past its deadline,
 * so callbacks MUST be bounded and non-hanging. A per-callback duration watchdog
 * warns past KWORKER_CALLBACK_WARN_MS. This is a single-worker background-monitor
 * primitive, NOT a parallel workqueue -- do not register work that may block for
 * seconds. */
int kworker_register(kworker_callback_t fn, void *ctx, uint32_t period_ms);

/* Cancel a registered callback. After this returns: the callback will not fire
 * again AND any in-flight call has drained, so the caller may safely free ctx.
 * Returns 0 on success, negative on a stale/invalid token.
 *
 * Re-entrancy: when called from WITHIN a kworker callback (i.e. on the worker
 * thread), it marks the slot inactive but does NOT wait for the in-flight call
 * to drain -- it cannot wait for itself. The current callback finishes normally;
 * no further fire occurs. */
int kworker_unregister(int token);

/* Create the shared kworker thread. Call ONCE from boot Phase 3 (after the
 * scheduler is up). Idempotent. Returns 0 on success (worker running), negative
 * if the worker task could not be created -- callers must NOT register monitors
 * when this fails (no thread would run them). */
int kworker_init(void);

/* Test/diagnostic: last fire time (uptime ns) for a token; 0 if never fired or
 * the token is stale/invalid. */
uint64_t kworker_last_fire_ns(int token);

/* Non-zero when the shared worker is RUNNING and will service registrations.
 *
 * This is a pure query with NO side effects: unlike kworker_init(), it never
 * creates the worker task and never yield-spins waiting for one to publish
 * STARTED. Use it when a caller needs to know whether registering is
 * worthwhile but must not pay a startup handshake -- notably any path that can
 * run on a latency-sensitive thread, where kworker_init() on a failed-startup
 * system would re-run task_create or spin to KWORKER_START_YIELD_CAP. A caller
 * that legitimately wants to START the worker still calls kworker_init(). */
int kworker_is_started(void);
