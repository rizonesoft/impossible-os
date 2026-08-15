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
 * the page -- publish, UN-PUBLISH, revoke, consume and validation DROP -- is a
 * single atomic store or CAS on that word, which is what lets a reader and a
 * concurrently-panicking writer agree on what the page holds. Before the epoch
 * existed, each consumer invented its own ad-hoc identity check (compare
 * boot_seq + crc, THEN clear the magic) and none of them was atomic against a
 * panic landing in between.
 *
 * DURABILITY IS PART OF THE CONTRACT, not an implementation detail. Atomic in
 * RAM is not enough for a record whose whole purpose is to be read after a
 * reset: the page is write-back memory and a reset invalidates caches without
 * writing them back, so a transition left in cache did not happen as far as
 * the next boot is concerned. Every SUCCESSFUL transition above is therefore
 * pushed to memory before the next lifecycle step, using the panic-safe
 * primitives in kernel/cache.h -- which the collector's "raw physical writes
 * only, no kmalloc / VFS / printk / spinlock" rule permits, because those
 * primitives allocate nothing, lock nothing and log nothing.
 *
 * The inverse edges matter as much as publication. A consume that never
 * reaches memory lets a reset resurrect the record it retired, so the next
 * boot re-reports a crash the user already saw -- and that stale record holds
 * the single slot against the crash that actually killed the machine.
 *
 * PUBLICATION ORDER: un-publish durably, write and flush the whole body while
 * the word reads zero, then store and flush the word. Ordering the flushes
 * behind an already-stored word is NOT equivalent; see kernel/cache.h. */
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
/* The _at() variants run the SAME 64-bit atomics against a caller-supplied page
 * (a test fixture in BSS, not 0x80000), so the TYPE must carry the alignment the
 * fixed address gives the live page. It holds today only implicitly, via the
 * uint64_t members. */
_Static_assert(_Alignof(struct panic_evidence) >= 8u,
               "any panic_evidence storage must be 8-byte aligned: the _at() "
               "helpers run 64-bit atomics on a caller-supplied page");
/* ARCH: x86-64 -- the publication word packs magic in the LOW half and epoch in
 * the HIGH half, which is the little-endian aliasing of the two leading uint32_t
 * fields. A big-endian port must swap the halves in ev_pubword_val, not just
 * recompile. */
_Static_assert((uint8_t)((uint16_t)1u) == 1u,
               "publication-word packing assumes a little-endian layout");
/* The CRC covers everything from boot_seq on, so every field the checksum is
 * meant to protect must sit AFTER it -- and the publication word must sit
 * outside that range, since publish and revoke rewrite it without touching the
 * CRC. */
_Static_assert(__builtin_offsetof(struct panic_evidence, boot_seq) >
               __builtin_offsetof(struct panic_evidence, crc32),
               "crc32 must precede the region it covers");

/* CHECKSUM: `kcrc32` (kernel/kchecksum.h) -- the collector and the Phase-0
 * restore both call it, so both sides compute identical checksums by sharing
 * one implementation rather than by two copies agreeing.
 *
 * The self-contained bit-at-a-time `panic_crc32` that used to live here was
 * retired: it cost about 53 instructions per byte over the whole ~3.2 KiB
 * record (~170k instructions) inside the window between the fault and a durable
 * record, against ~13 per two bytes for the table-driven routine already linked
 * into this kernel. Both are the reflected IEEE CRC-32 (poly 0xEDB88320,
 * init/xorout 0xFFFFFFFF), so the on-page format is unchanged and a record
 * written by an older kernel still validates -- asserted directly against the
 * retired algorithm in test_boot_diag.c, not reasoned about here. */

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
 * a no-op: the standing record already describes that fault.
 *
 * `ctx` is the panic context DERIVED ONCE at panic entry (panic_declared_ctx)
 * and passed down, not re-derived here. panic_declared_ctx reads the NMI depth
 * through cpu_panic_safe_apic_id(), i.e. a CPUID -- a serializing instruction
 * that exits to the hypervisor under KVM/WHPX -- and the entry context is the
 * one that must govern: it is the context of the fault being recorded, and the
 * invocation whose frames the guarded read could trample is the one already on
 * the stack. Passing it also makes the value the collector uses provably the
 * same one the panic-string snapshot was taken under. This is the CONTEXT half
 * of the same rule the IDENTITY half (`me`) already follows. */
void panic_collect_evidence(struct interrupt_frame *frame, uint32_t bugcheck_code,
                            const uint64_t bugcheck_params[4],
                            const char *message, const char *file, uint32_t line,
                            uint32_t me, uint32_t token, int terminal,
                            uint32_t spins_max, uint32_t ctx);

/* Populate a crash record IN PLACE, from `version` through `crc32` -- the
 * CONTENT half of panic_collect_evidence, split out so it can be exercised
 * against a fixture page. The collector's own body can only run on the live
 * 0x80000 page through panic_evidence_take, which mutates boot-global ownership
 * and would lock a later real panic out of the record, so the whole of record
 * population had no test surface at all.
 *
 * Writes NO publication state: arbitration, the epoch, the un-publish, the
 * zeroing and the final publication word all stay in the collector, which is
 * what keeps every lifecycle transition on the publication word atomic and in
 * one place. Callers other than the collector are test fixtures. */
void panic_evidence_populate(struct panic_evidence *ev,
                             struct interrupt_frame *frame,
                             uint32_t bugcheck_code,
                             const uint64_t bugcheck_params[4],
                             const char *message, const char *file,
                             uint32_t line, uint32_t me, uint32_t ctx);

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

#ifdef KERNEL_TESTS
/* Derivation counter for the section-24 assertion that a panic pays for the
 * context ONCE and hands it down. "Once" is not a property a signature can
 * carry: a later edit could re-derive it inside the collector exactly as the
 * original did, and every other test would still pass. Test builds only. */
uint32_t panic_declared_ctx_calls(void);
void     panic_declared_ctx_calls_reset(void);
#endif /* KERNEL_TESTS */

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

/* ---- Async-worker park: disposition and staged, at-most-once cleanup ----
 *
 * An AP that faults inside an async boot-init step is NOT terminal: the panic
 * path hands its emergency-serial allowances back, retracts the CPU from the
 * online mask, publishes the step failure so the BSP's barrier can fall back to
 * sequential init, emits one diagnostic, hands back the UART lock, revokes the
 * cross-boot crash record, and parks the CPU forever. The BSP keeps booting.
 *
 * THE TAIL IS RE-ENTRANT, because `cli` masks neither NMI nor #MC and every
 * step of it can itself fault. Two separate properties make that survivable.
 *
 * IDENTITY IS GS-INDEPENDENT. The stage lives in an array keyed by
 * cpu_panic_safe_apic_id(), the same CPUID-derived id the serial owner word,
 * the NMI depth and the crash evidence record already use, NOT in per_cpu_data.
 * smp_this_cpu() reads gs:0 and SUBSTITUTES the BSP slot when that read yields
 * 0, so it never reports failure: on a CPU whose GS is exactly what cannot be
 * trusted, a per-CPU flag would be read from, and written to, CPU 0's state,
 * and the parking CPU would still reach terminal arbitration.
 *
 * ONLY THE UNBOUNDED STEP IS CLAIMED, and which steps those are is the whole
 * cleanup-safety argument. Claiming a stage BEFORE a step means an abort in the
 * gap SKIPS that step forever, so it is right only where re-running is worse
 * than skipping. Exactly one step qualifies: the diagnostic, which walks a
 * caller-supplied string and drives the UART, and could therefore fault in the
 * same place on every entry and never reach the park. Every other step of the
 * tail -- the allowance reclaim, the online-mask retract, the UART hand-back,
 * the evidence revoke -- is a handful of compare-exchanges over kernel-owned
 * static state reached through a GS-independent slot. None can fault, all are
 * idempotent, and gating them would be strictly worse: an abort landing in
 * front of the UART hand-back would park this CPU still holding the serial lock
 * and hang every surviving CPU on its next write.
 *
 * The recursion is bounded by that split rather than by a depth counter: the
 * one step that could loop runs at most once, and nothing else in the tail can
 * fault twice in the same place. */
#define PANIC_PARK_NONE         0u  /* not parking: ordinary terminal arbitration */
#define PANIC_PARK_ENTERED      1u  /* the branch is committed to parking this CPU */
#define PANIC_PARK_PUBLISHED    2u  /* completion settled: published, or not owed */
#define PANIC_PARK_DIAGNOSED    3u  /* the one-record diagnostic was attempted */

/* What a panic entry on this CPU owes, given the async-worker state. The three
 * answers correspond exactly to the three intervals the park tail passes
 * through, which is why a boolean "am I parking" cannot express it:
 *
 *   TERMINAL  not an async worker and not parking: ordinary arbitration.
 *   ISOLATE   run the park tail INCLUDING the completion publication. The first
 *             entry, and any nested abort landing before the publication, where
 *             the BSP has been told nothing and would otherwise wait out its
 *             whole barrier deadline.
 *   PARK      run the park tail but SKIP the publication, because it already
 *             happened and the BSP is proceeding on the strength of it.
 *
 * `in_async_work` is checked FIRST and on its own: a CPU still identifying as
 * an async worker has not published, whatever else is true, so that reading can
 * never be talked out of publishing by a stale word.
 *
 * "Already published" is read off the STAGE and not off `pcpu->async_done`,
 * deliberately. The stage is the GS-independent copy, and taking both readings
 * would reintroduce through the back door exactly the per-CPU dependency the
 * stage exists to remove. Pure over its arguments, so all four rows are
 * unit-testable without a fault. */
#define PANIC_ASYNC_TERMINAL  0u
#define PANIC_ASYNC_ISOLATE   1u
#define PANIC_ASYNC_PARK      2u
uint32_t panic_async_disposition(int in_async_work, uint32_t park_stage);

/* Claim `step` in a monotonic park-stage slot: 1 = the caller now owns the step
 * and must perform it, 0 = some earlier entry already claimed it, skip it.
 *
 * Takes a CALLER-SUPPLIED slot, exactly as the serial lock-policy helpers take
 * a caller-supplied lock, so the at-most-once and monotonicity properties are
 * testable without touching the live per-CPU array or arming anything. A slot
 * only ever moves forward: an out-of-order claim of an already-passed step is
 * refused and leaves the slot unchanged. A NULL slot is refused rather than
 * dereferenced, because the panic path is the worst place to fault out of a
 * bookkeeping helper. */
int panic_park_claim_step(volatile uint8_t *slot, uint32_t step);

/* Append a CALLER-SUPPLIED string to a panic record buffer without ever
 * dereferencing it raw. Appends `(null)` for a NULL source, PANIC_STR_NO_GUARD
 * when `ctx` forbids the fault-suppressed read, and otherwise the guarded copy,
 * keeping the prefix and marking it when the pointer faults mid-walk.
 *
 * Public so the forbidden-context row is assertable. That row is the one that
 * regressed: it used to fall through to the RAW appender, whose `while (*s)`
 * is exactly the dereference this helper exists to replace -- and the entries
 * that take it are NMI and #DF, where a bad pointer is a triple fault and a
 * machine reset rather than a recoverable page fault. Pure over its arguments;
 * touches no lock and no boot state. */
void panic_append_guarded(char *buf, uint32_t cap, uint32_t *pos,
                          const char *s, uint32_t ctx);

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
int panic_evidence_take(uint32_t me, uint32_t token, int terminal,
                        uint32_t spins_max);

/* Revoke this CPU's published record on the survivable async-park path, and
 * release the page. Every mutation is generation-conditional, so a terminal
 * invocation that already took the page and republished is never disturbed. */
void panic_evidence_abandon(uint32_t me);

/* True when THIS invocation's terminal takeover could not establish writer
 * quiescence and the page still owes it a record; the terminal path retries
 * later and, if that also fails, says so on serial rather than losing the record
 * silently. Scoped to the token because the obligation belongs to one
 * invocation: a bare flag would be cleared by whatever unrelated panic next
 * acquires the page, cancelling a retry that had not happened. */
int panic_evidence_takeover_pending_for(uint32_t token);
