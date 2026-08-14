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
#define PANIC_EVIDENCE_VERSION  3u   /* v3: publication word carries an epoch */
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
 * version + size + a valid crc32 over everything-after-crc32 is trusted.
 *
 * `magic` and `epoch` are deliberately adjacent at offset 0 so the pair forms
 * ONE naturally-aligned 64-bit PUBLICATION WORD. Every lifecycle transition on
 * the page -- publish, revoke, consume -- is a single atomic store or CAS on
 * that word, which is what lets a reader and a concurrently-panicking writer
 * agree on what the page holds. Before the epoch existed, each consumer invented
 * its own ad-hoc identity check (compare boot_seq + crc, THEN clear the magic)
 * and none of them was atomic against a panic landing in between. */
struct panic_evidence {
    /* --- header (validated before anything else is trusted) --- */
    uint32_t magic;            /* PANIC_EVIDENCE_MAGIC -- published LAST */
    uint32_t epoch;            /* publication generation; see panic.c epoch rules */
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
/* The publication word. magic and epoch must be the FIRST two dwords and must
 * be contiguous, because publish/revoke/consume each act on them as one aligned
 * 64-bit location. Split them and every one of those transitions silently
 * degrades back into the non-atomic compare-then-clear this format replaced. */
_Static_assert(__builtin_offsetof(struct panic_evidence, magic) == 0u,
               "magic must start the publication word at page offset 0");
_Static_assert(__builtin_offsetof(struct panic_evidence, epoch) == 4u,
               "epoch must be the high half of the 64-bit publication word");
_Static_assert(PANIC_EVIDENCE_ADDR % 8u == 0u,
               "the evidence page must be 8-byte aligned for the publication word");
/* The CRC covers everything from boot_seq on, so every field the checksum is
 * meant to protect must sit AFTER it -- and the publication word must sit
 * outside that range, since publish and revoke rewrite it without touching the
 * CRC. */
_Static_assert(__builtin_offsetof(struct panic_evidence, boot_seq) >
               __builtin_offsetof(struct panic_evidence, crc32),
               "crc32 must precede the region it covers");

/* IEEE CRC-32 (self-contained; shared by collector + Phase-0 restore so both
 * sides compute identical checksums). */
uint32_t panic_crc32(const void *data, uint32_t len);

/* Collect crash evidence into the physical evidence page. Safe to call from a
 * faulted context: raw physical writes only. `frame` may be NULL (software
 * bugcheck with no exception frame). `bugcheck_params` is the emitting call's own
 * four STOP parameters (or NULL for a raw exception with none) -- passed by value
 * so the record cannot combine one CPU's frame with another CPU's parameters, the
 * way recovering them from the global g_last_bugcheck by code-equality could.
 * `message` and `file` are expected to be the panic-entry SNAPSHOT (see
 * panic_snapshot_str), not the caller's own pointers.
 *
 * A COMPLETED record is never overwritten, so a nested fault during BSOD render
 * cannot restate its own crash over the original. An INCOMPLETE one is not
 * protected, deliberately: the invocation that was writing it has been
 * interrupted by something terminal and is not coming back, so a later
 * invocation finishing a whole record is the only remaining way to leave any
 * evidence at all.
 *
 * `token` is this invocation's identity from panic_evidence_begin(). `terminal`
 * is 0 for the early capture (which runs before the panic path knows whether the
 * machine dies) and 1 only once panic_try_claim_owner() has declared this
 * invocation system-terminal -- which is what licenses it to take the page from
 * a survivable owner. An invocation that does not win the page writes nothing.
 * Calling with the SAME token over a record that invocation already published is
 * a no-op: the standing record already describes that fault. */
void panic_collect_evidence(struct interrupt_frame *frame, uint32_t bugcheck_code,
                            const uint64_t bugcheck_params[4],
                            const char *message, const char *file, uint32_t line,
                            uint32_t token, int terminal);

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

/* Frames the panic frame-chain walk will record at most, however large a count
 * panic_capture_frames is given. */
#define PANIC_MAX_STACK_DEPTH  16u

/* Placeholders the panic-entry snapshot emits INSTEAD of walking a pointer it
 * is not allowed to walk (see panic_snapshot_str). Public so a test can assert
 * the exact rendered text rather than a prefix. */
#define PANIC_STR_NO_GUARD    "(unavailable: no guarded read in this context)"
#define PANIC_STR_UNREADABLE  "(unreadable: the panic string pointer faulted)"
#define PANIC_STR_NONE        "(no description)"
#define PANIC_TRACE_NO_GUARD  "(stack trace withheld: no guarded read in this context)"

/* Copy a CALLER-SUPPLIED panic string into kernel-owned storage, ONCE, at panic
 * entry, so that every terminal renderer downstream (serial, BSOD, disk crash
 * dump, cross-boot evidence record) reads the copy and none of them walks a
 * pointer that may be the corruption being reported.
 *
 * Always NUL-terminates within `cap` (and writes nothing at all when `cap` is
 * 0). `src == NULL` renders `if_null`, or an empty string when `if_null` is
 * NULL too -- a NULL source is never read in ANY context; a `ctx`
 * that forbids the fault-suppressed read renders PANIC_STR_NO_GUARD without
 * touching `src` at all; a pointer unreadable from its FIRST byte renders
 * PANIC_STR_UNREADABLE. A partial read is kept as-is -- a truncated description
 * still names the crash. */
void panic_snapshot_str(char *dst, uint32_t cap, const char *src,
                        uint32_t ctx, const char *if_null);

/* Walk the frame-pointer chain with fault-suppressed reads, writing up to
 * `count` return addresses to out[] and returning how many were recorded.
 *
 * Returns 0 without touching memory when `ctx` forbids the guarded read: the
 * frame-chain walk is the documented route by which a nested #PF re-arms NMI
 * delivery over live IST frames (see panic_declared_ctx). Frames are validated
 * the same way the RtlCaptureStackBackTrace walker validates them -- 8-aligned,
 * within one bounded span above the interrupted RSP, strictly climbing, and
 * canonical. */
uint32_t panic_capture_frames(struct interrupt_frame *frame, uint32_t ctx,
                              uint64_t *out, uint32_t count);

/* Phase-0 restore: if the evidence page holds a valid record, copy it into the
 * caller-provided buffer and return 1; else 0. Pre-heap safe -- copies into
 * caller storage, never allocates.
 *
 * RESTORE DOES NOT CONSUME. The record is durable only once last-panic.txt has
 * been written, so restore RETAINS the page and is repeatable; only
 * panic_evidence_consume() clears it. The header used to promise the opposite
 * ("clear the page magic") while the code deliberately retained -- the same
 * class of defect as the non-atomic consume one layer down, and fixed with it.
 *
 * Reads are epoch-guarded: a record being rewritten by a concurrent panic on
 * another CPU is never returned half-copied. The copy is retried a bounded
 * number of times and then reported as no-record rather than as a torn one. */
int panic_evidence_restore(struct panic_evidence *out);

/* Fixture-page variants. Identical logic against a caller-supplied page, so the
 * lifecycle can be asserted in unit tests WITHOUT touching the live 0x80000 page
 * or the boot-global ownership words -- calling the live reserve/collect from a
 * test would lock a later real panic out of the record. */
int panic_evidence_restore_at(volatile struct panic_evidence *page,
                              struct panic_evidence *out);
void panic_evidence_consume_at(volatile struct panic_evidence *page, uint32_t epoch);
int panic_evidence_publish_at(volatile struct panic_evidence *page, uint32_t epoch);
int panic_evidence_revoke_at(volatile struct panic_evidence *page, uint32_t epoch);

/* Next publication generation, given the epoch standing on the page and the one
 * this boot restored. Never 0, and never equal to EITHER input, so a record
 * published now can never be mistaken for -- or erased in place of -- the record
 * that stood before it. Exposed for unit tests: the wraparound cases (a standing
 * epoch of UINT32_MAX from untrusted cross-boot RAM) are exactly the ones a
 * naive max()+1 gets wrong. */
uint32_t panic_evidence_next_epoch(uint32_t standing, uint32_t restored);

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
 * before the write retries on the next boot).
 *
 * ONE compare-and-swap on the publication word, keyed by the epoch this boot
 * restored: a record published by a concurrent panic between the check and the
 * clear carries a different epoch, so the CAS fails and that fresh crash
 * survives. The previous compare-then-clear could not express that -- it
 * compared boot_seq + crc and then cleared the magic as a separate store, and a
 * panic landing in the gap lost its record. */
void panic_evidence_consume(void);

/* --- cross-boot evidence lifecycle ----------------------------------------
 *
 * The page holds ONE record and a boot can produce several panics, only some of
 * which kill the machine. Ownership is therefore decided at TERMINAL
 * arbitration, not at panic entry, and the polarity is DEMOTE-ON-SURVIVAL: a
 * record is restorable from the instant it is published, and only the async
 * survival path revokes it. That way a machine that dies between collection and
 * arbitration still leaves the record it collected -- which is the whole reason
 * collection runs before the variable-latency panic work. */

/* Allocate this panic invocation's identity. Never 0, and distinct for a NESTED
 * invocation on the same CPU -- panic_try_claim_owner() is same-CPU re-entrant,
 * so an APIC id names a CPU, never the invocation that published a record. */
uint32_t panic_evidence_begin(void);

/* Try to become the invocation that owns the page. `terminal` is 1 only once
 * panic_try_claim_owner() has declared this invocation system-terminal, which is
 * what licenses it to TAKE the page from a survivable owner. Returns 1 when the
 * caller may write the page. */
int panic_evidence_take(uint32_t token, int terminal);

/* Revoke this CPU's published record on the survivable async-park path, and
 * release the page. Every mutation is generation-conditional, so a terminal
 * invocation that already took the page and republished is never disturbed. */
void panic_evidence_abandon(void);

/* True when THIS invocation's terminal takeover could not establish writer
 * quiescence and the page still owes it a record; the terminal path retries
 * later and, if that also fails, says so on serial rather than losing the record
 * silently. Scoped to the token because the obligation belongs to one
 * invocation: a bare flag would be cleared by whatever unrelated panic next
 * acquires the page, cancelling a retry that had not happened. */
int panic_evidence_takeover_pending_for(uint32_t token);
