/* ============================================================================
 * apc.h -- Asynchronous Procedure Call (KAPC) objects + per-thread APC queues
 *
 * APCs are the per-thread deferred-work mechanism at APC_LEVEL. Kernel APCs
 * run I/O completion / thread cleanup / cross-thread injection; user APCs
 * deliver async callbacks to alertable threads. This header defines the KAPC
 * object, the per-thread KAPC_STATE (two queues + pending flags), the queue
 * insert/remove API, and critical/guarded-region gating. The DELIVERY engine
 * (KiDeliverApc, interrupt-return wiring) is a separate section -- this file is
 * the data structures + queue ops + region counters only.
 *
 * Mirrors the KDPC subsystem (kernel/sched/dpc.h): caller-owned intrusive
 * objects, a per-thread irqsave spinlock, no allocation on the insert path.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/sched/spinlock.h"

/* APC processor mode: which ring the NormalRoutine runs for. */
typedef enum {
    ApcKernelMode = 0,
    ApcUserMode   = 1,
} KPROCESSOR_MODE_APC;

/* APC environment / which APC state the APC targets (original vs attached). */
typedef enum {
    OriginalApcEnvironment = 0,
    AttachedApcEnvironment = 1,
    CurrentApcEnvironment  = 2,   /* resolve to the thread's current state */
} KAPC_ENVIRONMENT;

struct _KAPC;

/* Kernel routine: runs at APC_LEVEL before the NormalRoutine; also the cleanup
 * hook (may free the KAPC). NULL is legal for a bare special kernel APC. */
typedef void (*PKKERNEL_ROUTINE)(struct _KAPC *apc, void **normal_routine,
                                 void **normal_context, void **system_arg1,
                                 void **system_arg2);
/* Rundown routine: runs if the APC is still queued when the thread exits. */
typedef void (*PKRUNDOWN_ROUTINE)(struct _KAPC *apc);
/* Normal routine: the actual deferred work (kernel APC at PASSIVE, user APC in
 * ring 3). NULL = a special kernel APC (KernelRoutine only). */
typedef void (*PKNORMAL_ROUTINE)(void *normal_context, void *system_arg1,
                                 void *system_arg2);

/* ---- KAPC object (caller-owned; intrusive queue link) ------------------- */

typedef struct _KAPC {
    uint8_t            type;            /* object type tag (ApcObject) */
    uint8_t            size;            /* sizeof(KAPC) for validation */
    uint8_t            apc_state_index; /* which KAPC_STATE this targets */
    uint8_t            apc_mode;        /* KPROCESSOR_MODE_APC */
    uint8_t            inserted;        /* 1 = currently queued */
    void              *thread;          /* target struct thread* */
    struct _KAPC      *next;            /* intrusive queue link (NULL = tail) */
    PKKERNEL_ROUTINE   kernel_routine;
    PKRUNDOWN_ROUTINE  rundown_routine;
    PKNORMAL_ROUTINE   normal_routine;  /* NULL => special kernel APC */
    void              *normal_context;
    void              *system_arg1;     /* per-insertion argument 1 */
    void              *system_arg2;     /* per-insertion argument 2 */
} KAPC;

/* ApcObject type tag (KAPC.type). */
#define APC_OBJECT_TYPE  0x12

/* ---- KAPC_STATE (embedded in each thread; swappable for attach) --------- */

/* Two APC queues + pending flags. This struct is SWAPPABLE (ApcState <->
 * SavedApcState during a future KeStackAttachProcess); the critical/guarded
 * region nesting counters are deliberately kept OUTSIDE it (separate per-thread
 * fields) so an attach/detach cannot move region state and re-enable APC
 * classes at the wrong time. */
typedef struct _KAPC_STATE {
    KAPC              *apc_list_head[2];   /* [ApcKernelMode], [ApcUserMode] */
    void              *process;            /* owning struct task* */
    uint8_t            kernel_apc_pending;
    uint8_t            user_apc_pending;
    uint8_t            special_user_apc_pending;
    uint8_t            kernel_apc_in_progress;
} KAPC_STATE;

/* ---- API ---------------------------------------------------------------- */

/* Initialize a KAPC. normal_routine NULL => special kernel APC. apc_mode is
 * KPROCESSOR_MODE_APC. The KAPC is caller-owned (static/pool/heap). */
void KeInitializeApc(KAPC *apc, void *thread, KAPC_ENVIRONMENT environment,
                     PKKERNEL_ROUTINE kernel_routine,
                     PKRUNDOWN_ROUTINE rundown_routine,
                     PKNORMAL_ROUTINE normal_routine, uint8_t apc_mode,
                     void *normal_context);

/* Queue an APC to its target thread (set at init). Special kernel APCs go to
 * the head of the kernel queue; normal/user APCs to the tail; sets the matching
 * pending flag. Under the target thread's APC lock. Returns 1 on success, 0 if
 * the thread is exiting (THREAD_DEAD/FREE) or the APC is already queued.
 * ISR-safe: no allocation, irqsave-locked. */
int KeInsertQueueApc(KAPC *apc, void *system_arg1, void *system_arg2,
                     uint8_t increment);

/* Remove a queued APC before delivery. Returns 1 if it was queued (and is now
 * removed), 0 if not. Under the target thread's APC lock. */
int KeRemoveQueueApc(KAPC *apc);

/* ---- Critical / guarded regions (per-thread, on thread_current()) -------- */

/* Critical region: blocks NORMAL kernel APC delivery (++KernelApcDisable). */
void KeEnterCriticalRegion(void);
void KeLeaveCriticalRegion(void);
/* Guarded region: blocks ALL kernel APC delivery incl special kernel APCs
 * (++SpecialApcDisable). */
void KeEnterGuardedRegion(void);
void KeLeaveGuardedRegion(void);

/* TRUE inside a critical OR guarded region (any kernel APC class disabled). */
int KeAreApcsDisabled(void);
/* TRUE inside a guarded region only (all kernel APC classes disabled). */
int KeAreAllApcsDisabled(void);

/* Per-thread APC-state init helper (called from thread create / slot reuse,
 * zeros both queues + flags; the caller holds nothing -- this runs before the
 * thread is schedulable). Defined in apc.c, declared here for task.c. */
void apc_thread_init(KAPC_STATE *apc_state, void *process);
