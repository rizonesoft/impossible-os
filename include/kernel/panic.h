/* ============================================================================
 * panic.h -- Kernel panic screen
 *
 * Provides a styled graphical panic screen (blue screen of death) with:
 *   - Exception name and stop code
 *   - Faulting address and RIP
 *   - Source file + line (via __FILE__, __LINE__)
 *   - Full register dump (RAX–R15, RSP, RFLAGS, CR2, CR3)
 *   - Stack trace (RBP chain walk)
 *   - Auto-restart countdown (configurable via Registry)
 *   - Crash dump to C:\Impossible\System\crashdump.log
 * ============================================================================ */

#pragma once

#include "kernel/klog.h"  /* KLOG_SUBSYSTEM_MAX pins the serialized tag field */

#include "kernel/types.h"

/* Forward declaration */
struct interrupt_frame;

/* The Windows AMD64 exception ABI (CONTEXT, XMM_SAVE_AREA32, M128A, the
 * CONTEXT_* group flags) is owned by except.h. It lived here while crash
 * dumps were its only consumer; the layout is unchanged. */
#include "kernel/except.h"

/* --- XSAVE scratch buffer (exported for tests) --------------------------- */

extern uint8_t g_panic_xsave_buf[4096];

/* --- Crash CONTEXT (exported for dump pipeline) -------------------------- */

extern CONTEXT g_panic_context;

/* Capture FPU/XMM/YMM state into g_panic_xsave_buf.
 * Uses XSAVE if available, FXSAVE fallback otherwise.
 * Must be called as early as possible in the panic path. */
void panic_capture_fpu_state(void);

/* Build a CONTEXT record from interrupt_frame + captured XSAVE state. */
void panic_build_context(struct interrupt_frame *frame, CONTEXT *ctx);

/* Display the styled panic screen and halt.
 * frame: interrupt frame snapshot (NULL if not from an exception)
 * error_code: exception error code or stop code
 * description: human-readable error description
 * file: source file (__FILE__)
 * line: source line (__LINE__) */
void panic_screen(struct interrupt_frame *frame, uint64_t error_code,
                  const char *description, const char *file, uint32_t line);

/* Convenience macro that captures file/line automatically.
 * Routes through KeBugCheckEx for uniform bugcheck handling.
 * Usage: KPANIC("something terrible happened"); */
#define KPANIC(msg) \
    do { \
        extern void KeBugCheckEx(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t); \
        KeBugCheckEx(0xE2, 0, 0, 0, 0); /* MANUALLY_INITIATED_CRASH */ \
    } while (0)

/* Panic with an interrupt frame (called from exception handlers).
 * Still uses panic_screen directly for frame-aware BSOD rendering.
 * Usage: KPANIC_FRAME(frame, "page fault in kernel"); */
#define KPANIC_FRAME(frame, msg) \
    panic_screen((frame), (frame)->err_code, (msg), __FILE__, __LINE__)

/* ============================================================================
 * Panic forensic evidence -- the Linux pstore/ramoops
 * equivalent. At fault time, panic_collect_evidence() snapshots crash identity
 * + a small payload into a fixed physical page that survives a WARM reboot
 * (RAM retained); Phase 0 validates and restores it on the next boot.
 *
 * The collector runs in a faulted, possibly-unstable context, so it does ONLY
 * raw physical writes to the identity-mapped evidence page -- no kmalloc, VFS,
 * printk, or spinlock -- and reads GPRs from the passed interrupt_frame.
 * History/klog strings are copied INLINE at collect time (while the source
 * pointers are still valid), never stored as pointers that would dangle across
 * a reboot.
 * ========================================================================== */

#define PANIC_EVIDENCE_MAGIC    0xDEADBEEFu
#define PANIC_EVIDENCE_VERSION  2u   /* v2: klog entries carry pid/tid */
#define PANIC_EVIDENCE_ADDR     0x80000u   /* fixed physical page; reserved by PMM */
#define PANIC_EVIDENCE_STAGES   16u        /* last N boot-stage entries captured */
#define PANIC_EVIDENCE_KLOGS    8u         /* last N klog ring entries captured */

/* Serialized (pointer-free) boot-stage entry -- inline msg, safe cross-boot. */
struct panic_stage_entry {
    uint32_t stage;
    uint32_t elapsed_ms;
    char     msg[48];
};

/* Serialized (pointer-free) klog entry -- inline subsystem + message. */
struct panic_klog_entry {
    uint32_t level;
    uint32_t timestamp;
    uint8_t  cpu_id;
    uint8_t  _pad[3];
    uint32_t pid;
    uint32_t tid;
    char     subsystem[KLOG_SUBSYSTEM_MAX];
    char     message[176];
};

/* The cross-boot evidence record. A versioned header (magic+version+size+crc32
 * +boot_seq) lets Phase-0 reject stale 0x80000 contents: only magic + matching
 * version + size + a valid crc32 over everything-after-crc32 is trusted. */
struct panic_evidence {
    /* --- header (validated before anything else is trusted) --- */
    uint32_t magic;            /* PANIC_EVIDENCE_MAGIC */
    uint32_t version;          /* PANIC_EVIDENCE_VERSION */
    uint32_t size;             /* sizeof(struct panic_evidence) */
    uint32_t crc32;            /* IEEE CRC32 over all bytes AFTER this field */
    uint32_t boot_seq;         /* boot_history seq of the crashed boot */

    /* --- crash identity --- */
    uint32_t bugcheck_code;
    uint64_t bugcheck_params[4];
    uint64_t fault_vector;     /* int_no */
    uint64_t err_code;
    uint64_t cr2;
    uint64_t cr0, cr3, cr4;
    uint32_t cpu_id;
    uint32_t line;             /* source line */
    uint64_t pmm_free_pages;
    uint64_t irq_mask;
    /* register file (from interrupt_frame) */
    uint64_t rip, rsp, rflags, cs, ss;
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    char     file[64];         /* source file */
    char     message[256];     /* panic message */

    /* --- payload --- */
    uint32_t post_code;        /* last POST16 code */
    uint32_t stage_count;
    uint32_t klog_count;
    uint32_t _pad;
    struct panic_stage_entry stages[PANIC_EVIDENCE_STAGES];
    struct panic_klog_entry  klogs[PANIC_EVIDENCE_KLOGS];
};

_Static_assert(sizeof(struct panic_evidence) <= 4096,
               "panic_evidence must fit one 4 KiB page (0x80000)");
_Static_assert(sizeof(struct panic_evidence) % 8u == 0u,
               "panic_evidence size must be a multiple of 8 (uint64 zero/copy loops)");

/* IEEE CRC-32 (self-contained; shared by collector + Phase-0 restore so both
 * sides compute identical checksums). */
uint32_t panic_crc32(const void *data, uint32_t len);

/* Collect crash evidence into the physical evidence page. Safe to call from a
 * faulted context: raw physical writes only. `frame` may be NULL (software
 * bugcheck with no exception frame). `bugcheck_params` is the emitting call's own
 * four STOP parameters (or NULL for a raw exception with none) -- passed by value
 * so the record cannot combine one CPU's frame with another CPU's parameters, the
 * way recovering them from the global g_last_bugcheck by code-equality could.
 * First caller wins per panic so a nested fault during BSOD render does not
 * overwrite the original record. */
void panic_collect_evidence(struct interrupt_frame *frame, uint32_t bugcheck_code,
                            const uint64_t bugcheck_params[4],
                            const char *message, const char *file, uint32_t line);

/* The declared panic context for this entry: PANIC_CTX_NMI when the
 * fault-suppressed kernel read must NOT be used, PANIC_CTX_NORMAL otherwise.
 *
 * Derived from TWO hardware signals, never probed from memory a panic may have
 * corrupted: the entry vector, AND this CPU's NMI nesting depth (idt_in_nmi()).
 * The vector alone cannot see NESTING -- a fault taken inside the NMI handler
 * re-enters panic naming the INNER vector, and classifying that as ordinary
 * re-enables a guarded read whose fixup IRETQs and re-arms NMI while the outer
 * NMI still owns IST2. A NULL frame is a software panic, which is never NMI
 * BY VECTOR but may still be nested inside one, so the depth test applies there
 * too. Exposed (rather than static) so the vector/depth/NULL truth table is
 * unit-testable without invoking any panic infrastructure. */
uint32_t panic_declared_ctx(struct interrupt_frame *frame);

/* Phase-0 restore: if the evidence page holds a valid record, copy it into the
 * caller-provided buffer, clear the page magic, and return 1; else 0. Pre-heap
 * safe -- copies into caller storage, never allocates. */
int panic_evidence_restore(struct panic_evidence *out);

/* Phase-0 hook: restore the evidence page into kernel-side storage and log if a
 * prior crash was found. Call early in boot_hw_init, before heap is up. */
void panic_evidence_restore_early(void);

/* True if panic_evidence_restore_early() found a valid prior-crash record this
 * boot. The desktop reads this to surface an "unexpected shutdown" notice. */
int panic_had_previous_crash(void);

/* Post-VFS emission of the restored record to X:\Crash\last-panic.txt (BlackBox)
 * or the C:\ fallback. Best-effort; no-op when no prior crash was restored. On
 * a successful write it calls panic_evidence_consume() so the same crash is not
 * re-reported next boot; a failed/skipped write leaves the page for retry. */
void panic_evidence_write_blackbox(void);

/* Clear the evidence page magic. Called after a successful last-panic.txt write
 * (the record is only consumed once durably persisted, so a boot that dies
 * before the write retries on the next boot). */
void panic_evidence_consume(void);
