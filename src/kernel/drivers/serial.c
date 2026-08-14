/* ============================================================================
 * serial.c -- COM1 serial port driver
 *
 * Extracted from main.c for reuse by printk and other subsystems.
 * ============================================================================ */

#include "kernel/drivers/serial.h"
#include "kernel/drivers/serial_emergency.h"
#include "kernel/boot_info.h"
#include "kernel/sched/spinlock.h"
#include "kernel/sched/irql.h"          /* KIRQL / DISPATCH_LEVEL -- serial_lock IRQL */
#include "kernel/smp.h"                 /* smp_this_cpu -- IRQL only, never identity */
#include "kernel/cpu_security.h"        /* __kread_u8 -- guarded caller-string walk */

/* --- 16550 register offsets from the port base ---------------------------- */
#define UART_REG_THR    0   /* transmit holding      (DLAB=0, write) */
#define UART_REG_RBR    0   /* receive buffer        (DLAB=0, read)  */
#define UART_REG_DLL    0   /* divisor latch low     (DLAB=1) */
#define UART_REG_IER    1   /* interrupt enable      (DLAB=0) */
#define UART_REG_DLH    1   /* divisor latch high    (DLAB=1) */
#define UART_REG_FCR    2   /* FIFO control */
#define UART_REG_LCR    3   /* line control */
#define UART_REG_MCR    4   /* modem control */
#define UART_REG_LSR    5   /* line status */

/* --- register bit values -------------------------------------------------- */
#define UART_LCR_8N1    0x03  /* 8 data bits, no parity, 1 stop; DLAB clear */
#define UART_LCR_DLAB   0x80  /* divisor latch access */
#define UART_LSR_DR     0x01  /* data ready */
#define UART_LSR_THRE   0x20  /* transmit holding register empty */
#define UART_FCR_INIT   0xC7  /* enable + clear both FIFOs, 14-byte trigger */
#define UART_MCR_INIT   0x0B  /* DTR + RTS + OUT2 (OUT2 gates the IRQ line) */

/* Baud divisor for the 115200 Hz reference clock. */
#define UART_BASE_CLOCK 115200u
#define UART_DIV_38400  3     /* fallback for an unrecognized SPCR baud code */

/* --- emergency-path bounds ------------------------------------------------ */

/* LSR-THRE polls per byte before a TERMINAL caller stops waiting on that byte.
 * The normal path spins here unbounded, which is what hangs a panic on a wedged
 * UART.
 *
 * COST: a 16550 at 0x3F8 is decoded over LPC/ISA at roughly 0.5-1 us per read,
 * so one full-length wait is ~33-65 ms and one budget of eight is ~0.26-0.52 s.
 * A panic can spend TWO such budgets, not one: the reason/register dump runs
 * before ownership is claimed and charges the budget as it stands, then the
 * publishing CAS clears it so post-claim output starts fresh. The worst case is
 * therefore ~1M polls, ~0.52-1.05 s, and that split is deliberate -- the clear
 * is what stops a survivable event from silencing the next real panic. (An earlier revision of this comment claimed 65536 reads was
 * "sub-millisecond", which would require ~15 ns per inb -- DRAM latency, not
 * port-I/O latency. The constant was justified by wrong arithmetic; the real
 * figure is stated here so the trade is visible.) That is acceptable ONLY
 * because the machine is already dying; a recoverable caller uses the far
 * shorter bound below. A healthy UART drains a character in ~87 us at 115200
 * and ~1 ms at 9600, so it never approaches this cap and never records a
 * timeout at all. */
#define SERIAL_EMERG_THRE_SPINS   65536u

/* LSR-THRE polls per byte for a RECOVERABLE (non-armed) caller -- today the WER
 * fault hook. ~2-4 ms, a few character times at the slowest configured baud, so
 * a healthy UART never reaches it. A recoverable path must not mask interrupts
 * or hold g_serial_lock for the ~0.5 s the terminal budget permits: the system
 * survives a user fault, and other CPUs entering ordinary serial writes would
 * spin on the lock with their own interrupts disabled. */
#define SERIAL_RECOV_THRE_SPINS   4096u

/* Timed-out bytes a RECOVERABLE caller will pay for within ONE call before
 * dropping to a single probe for the rest of that record. Per-call, so it is
 * naturally fresh and cannot be spent by an earlier fault. */
#define SERIAL_RECOV_STUCK_BYTES  2u

/* Full-length waits a TERMINAL epoch will pay for in total, after which every
 * further byte costs ONE status read instead of a spin.
 *
 * The budget spans the whole panic, NOT a single call, and that is the point:
 * panic.c emits its register dump through hundreds of separate calls, so a
 * per-call counter bounds nothing.
 *
 * MONOTONIC IN TIMEOUTS WITHIN AN EPOCH (see the two-phase note above: arming
 * clears it once, which starts the second phase). A reservation that times out
 * is never given back, and nothing else ever lowers the count -- a byte that drains returns only its own
 * reservation. An earlier revision zeroed the counter on every successful byte,
 * on the theory that a slow-but-working UART needed healing; that was both
 * unnecessary and harmful. Unnecessary because a UART that drains within the
 * spin cap never records a timeout in the first place, so there was nothing to
 * heal. Harmful because it made the bound meaningless in exactly the case this
 * code exists for: an INTERMITTENTLY draining transmitter (drain, timeout,
 * drain, timeout) got a fresh full-length wait after every successful
 * character, so the stall grew with output length instead of being capped. It
 * also let one CPU's success erase reservations other CPUs were still holding.
 *
 * SERIAL_EMERG_STUCK_BYTES is declared in serial_emergency.h: it is both the
 * advertised ceiling that panic.c's stall reasoning depends on and the extent of
 * the claim array, so a test that asserts the whole budget is reachable has to
 * use the real number rather than a copy of it. */

/* Hard cap on characters consumed from one emergency string. The C-string walk
 * runs on a path where the caller's pointer may itself be part of the
 * corruption being reported (a clobbered panic description or __FILE__), so an
 * unterminated buffer would otherwise read arbitrary memory until it happened
 * to hit a zero byte or an unmapped page. (The walk itself is done with the
 * lock NOT held -- see serial_write_emergency -- so a fault there is survivable;
 * this cap bounds how much it reads before giving up.)
 * The cap bounds how FAR the walk reads; surviving a pointer that is unmapped
 * rather than unterminated is a separate guarantee, and `__kread_u8` now
 * provides it (see serial_emergency_write_str). An earlier revision of this
 * comment claimed a fault-suppressed read was impossible here because the
 * RIP-keyed fixup table was user-range only -- that was never true: the kernel
 * read `__kstack_read_u64` is matched by RIP and direction with no CR2 gate.
 * The longest string reaching here in-tree is panic.c's 320-byte description
 * buffer, so this leaves 3x headroom.
 *
 * SERIAL_EMERG_MAX_CHARS and SERIAL_EMERG_CHUNK are declared in
 * serial_emergency.h rather than here, because the walk that divides a record by
 * them is only testable against the numbers it actually uses -- a test carrying
 * its own copy of 128 would keep passing after the chunk size changed. */

/* The guarded chunk buffer is SERIAL_EMERG_CHUNK + 1: __kstr_read_guarded
 * reserves one byte of its `cap` for a terminator, so copying a FULL chunk of
 * payload needs cap = SERIAL_EMERG_CHUNK + 1 and a destination to match. The
 * two sides are written in different functions and the fit is exact, with the
 * buffer living on a #DF/#MC IST stack -- the one context where an overflow
 * cannot report itself -- so the relationship is pinned here rather than left
 * to whoever next edits either side. */
#define SERIAL_EMERG_CHUNK_BUF    (SERIAL_EMERG_CHUNK + 1u)
_Static_assert(SERIAL_EMERG_CHUNK_BUF == SERIAL_EMERG_CHUNK + 1u,
               "the guarded chunk buffer must hold a full chunk plus the "
               "terminator __kstr_read_guarded reserves inside its cap");
_Static_assert(SERIAL_EMERG_CHUNK <= SERIAL_EMERG_MAX_CHARS,
               "a chunk cannot exceed the total emergency character budget");

/* Bounded retries for the full-wait reservation CAS -- see
 * serial_emerg_reserve. Contention losers degrade to a single status probe. */
#define SERIAL_EMERG_CAS_TRIES    64u

/* Serial port I/O base -- read from boot_info at serial_init().
 * Default to COM1 (0x3F8) for safety during early klog before init. */
static uint16_t s_serial_port = 0x3F8;

/* Protects UART register access from concurrent threads and IRQ handlers, and
 * records WHO holds it in the SAME word (serial_emergency.h).
 *
 * The previous shape was a spinlock_t plus a separate owner word. A CPU that
 * faults while holding this lock and then PARKS FOREVER -- exactly what the
 * panic async-isolation branch does to a faulting async-init worker (panic.c) --
 * would strand the flag set and block every surviving CPU on its next ordinary
 * serial write: a silent hang instead of a boot. The owner word broke that by
 * letting the parking CPU hand the UART back, but only NARROWED it, because the
 * owner store was a separate instruction from the flag CAS: an abort landing
 * between them left the lock held with owner 0, which the force-release then
 * correctly refused to touch, reproducing the very hang. Fusing the two into one
 * owner-encoded word closes it -- there is no longer an instruction boundary at
 * which the lock is held by nobody.
 *
 * Identified by the CPUID-derived id, not smp_this_cpu(), for the same reason
 * the emergency ledger is: the panic path that consumes this must not depend on
 * a GS base it cannot trust. That is not merely a preference here -- smp_this_cpu
 * falls back to &cpu_data[0] when GS is unset (smp.c), so an AP with no valid GS
 * would record the BSP as owner and the real BSP could then force-release the
 * AP's LIVE lock. The raw CPUID has no such failure mode. */
/* Cacheline-SIZED, not merely aligned-start. Every ordinary serial acquisition
 * takes this word for exclusive ownership, so anything sharing its line is
 * dragged along with it. Measured on the linked artifact before this padding:
 * the lock at 0x543540, the emergency latch s_emergency at 0x543544 and the
 * charge-generation ledger at 0x543550 all sat inside ONE 64-byte line -- and
 * the ordinary path READS that latch per byte, so every klog line on one CPU
 * was invalidating a line other CPUs were reading. The trailing pad reserves the
 * rest of the line so the mutable state declared after it cannot drift back into
 * false sharing. Same idiom as the wall-clock floor (time/wall_clock.c). */
#define SERIAL_CACHELINE 64u
_Static_assert(sizeof(serial_lock_t) <= SERIAL_CACHELINE,
               "serial lock must fit inside one cache line");
static struct {
    serial_lock_t lock;
    uint8_t       _pad[SERIAL_CACHELINE - sizeof(serial_lock_t)];
} __attribute__((aligned(SERIAL_CACHELINE))) g_serial_lock_cl = {
    .lock = SERIAL_LOCK_INIT,
    ._pad = { 0 }
};
/* Every existing call site reads naturally through this name; the padding is a
 * layout property, not part of the lock's interface. */
#define g_serial_lock (g_serial_lock_cl.lock)

/* Emergency latch states. Three, not two, so that ARMING itself is the one-shot:
 * only the CPU that wins the OFF->INIT transition records the owner and clears
 * the budget, and only it then release-stores ARMED. Every other entrant does
 * nothing. That is what makes the publication honest -- the sole way to observe
 * ARMED is through the initializer's release store, so an acquire reader always
 * sees the owner and the cleared budget with it. A two-state latch could not do
 * this: losing entrants also stored 1, and an observer synchronizing with a
 * LOSER's store saw none of the winner's relaxed initialization. */
#define SERIAL_EMERG_OFF    0x00000000u
#define SERIAL_EMERG_INIT   0x40000000u
#define SERIAL_EMERG_ARMED  0x80000000u
#define SERIAL_EMERG_STATE  0xC0000000u   /* state bits */
/* Wedged-UART allowances do NOT live in this word. Each one is a composite claim
 * word of its own in s_emerg_claim[] below, and the reason is the whole of this
 * comment.
 *
 * The obvious encoding is a slot BITMAP here, one bit per outstanding
 * full-length wait, and that is what this word carried until the composite claim
 * replaced it. A bitmap already beat a plain count: a count makes every
 * reservation in an epoch anonymous, so a return issued twice decrements some
 * OTHER CPU's outstanding wait and erases a timeout that really happened, and a
 * saturating zero check only stops the aggregate going negative, which is a
 * different property.
 *
 * But a bitmap bit names a SLOT and not an OWNER, so the owner had to be
 * recorded in a second word -- a per-CPU advisory ledger -- and `local_irq_save`
 * masks neither NMI nor #MC. An abort landing between the bitmap CAS and the
 * ledger write left an outstanding charge that no `charges_self` could
 * attribute and no refund could reclaim: one full-length wait, charged to
 * nobody, standing for the rest of the pre-arm phase.
 *
 * Claiming a composite {generation, owner} FIRST and publishing the bitmap bit
 * afterwards does not fix that. It converts the anonymous charge into an equally
 * uncounted claim leak: an abort in the new window leaves a same-generation
 * claim that no contender can safely distinguish from a live publisher, and a
 * contender that reclaims it anyway can erase a charge that is about to be
 * published. Two words and one abort window, in either order.
 *
 * So there is exactly ONE word per allowance and it is authoritative: the single
 * compare-exchange that takes the allowance is the same one that records who
 * owns it and which epoch it belongs to. There is no window because there is no
 * second step. A charge exists if and only if a live claim exists, which is what
 * serial_emerg_waits, serial_emerg_charges_self and the async refund each read
 * directly instead of reconstructing from two sources that can disagree.
 *
 * Tokens are SINGLE-USE, not idempotent -- a slot index is reused as soon as it
 * is freed, so an old token can match a new reservation. See serial_emerg_return
 * for why the encoding cannot do better in 32 bits, and why every caller in the
 * tree returns exactly once. */
#define SERIAL_EMERG_RSVD   0x0FF00000u   /* was the slot bitmap; now reserved */
#define SERIAL_EMERG_CPU    0x000000FFu   /* owner (8-bit initial APIC ID) */

/* Accounting-epoch generation, bumped by the SAME compare-exchange that
 * publishes ARMED and zeroes the budget -- so "which epoch is this charge in"
 * is published atomically with the epoch itself, exactly like the owner.
 *
 * It exists because the budget field alone cannot say WHEN a charge was taken.
 * A reservation made before the publish and handed back after it decremented
 * the NEW epoch, replenishing a live panic budget by up to a full ceiling. The
 * generation makes a return VALIDATABLE: a token minted under generation G only
 * decrements while the latch still reads G.
 *
 * 8 bits, which is far more than the one-way OFF->INIT->ARMED latch needs in
 * production (it arms exactly once per boot); the width is for
 * serial_emerg_reset_for_test, which starts a fresh accounting epoch on every
 * reset and would otherwise alias generations within a single test run.
 *
 * WRAP IS OUT OF CONTRACT rather than proven harmless, which is the honest
 * statement: after 256 epochs a retained token's generation aliases, and
 * returning it would release whatever now holds its slot. That is unreachable in
 * production -- the latch is one-way and arms at most once per boot, so the
 * generation advances at most once -- and reachable only by a test that performs
 * 256 resets while holding a token across them. Do not write that test expecting
 * the stale token to be rejected; it is not a supported case. */
#define SERIAL_EMERG_GEN    0x000FF000u   /* accounting epoch generation */
#define SERIAL_EMERG_GSHIFT 12u

#define serial_emerg_state(v)   ((v) & SERIAL_EMERG_STATE)
#define serial_emerg_cpu(v)     ((v) & SERIAL_EMERG_CPU)
#define serial_emerg_gen(v)     (((v) & SERIAL_EMERG_GEN) >> SERIAL_EMERG_GSHIFT)
#define SERIAL_EMERG_GEN_MAX    (SERIAL_EMERG_GEN >> SERIAL_EMERG_GSHIFT)

/* The fields share one word; an overlap would silently corrupt the owner or the
 * state every time another field changed. The reserved span is asserted against
 * the live fields too, so re-using those bits for something new cannot quietly
 * alias the state, the generation or the owner. */
_Static_assert((SERIAL_EMERG_STATE & SERIAL_EMERG_CPU) == 0u,
               "emergency latch: state and owner fields overlap");
_Static_assert((SERIAL_EMERG_RSVD & (SERIAL_EMERG_STATE | SERIAL_EMERG_CPU |
                                     SERIAL_EMERG_GEN)) == 0u,
               "emergency latch: reserved span overlaps a live field");
_Static_assert((SERIAL_EMERG_GEN & (SERIAL_EMERG_STATE | SERIAL_EMERG_CPU)) == 0u,
               "emergency latch: generation overlaps state or owner");
_Static_assert(SERIAL_EMERG_INIT != SERIAL_EMERG_ARMED &&
               SERIAL_EMERG_OFF != SERIAL_EMERG_INIT,
               "emergency latch: states must be distinct");

/* The UART lock owner, the emergency latch owner and the panic-safe id helper
 * must mask identity to the SAME width. They are three separate constants in
 * three headers, and a divergence would not fail any build or any test: it would
 * simply make the lock's compare-exchange stop matching for the ids above the
 * narrower mask, and the handback would degrade back to the hang it exists to
 * remove -- silently, and only on machines with enough CPUs to reach those ids.
 * Pin them to each other instead of trusting three copies of 0xFF. */
_Static_assert(SERIAL_LOCK_ID_MASK == SERIAL_EMERG_CPU,
               "serial lock owner and emergency latch owner must mask alike");
_Static_assert(SERIAL_LOCK_ID_MASK == CPU_PANIC_SAFE_ID_MASK,
               "serial lock owner must mask exactly what cpu_panic_safe_apic_id returns");
/* The encoding is id + 1, so no valid owner can collide with FREE, and the
 * widest id must still fit the word the lock is stored in.
 *
 * The fit check is computed in a WIDER type on purpose. SERIAL_LOCK_OWNER_OF is
 * uint32_t arithmetic, so `OWNER_OF(mask) <= 0xFFFFFFFFu` is true by
 * construction -- it is a guard that cannot fail, and it would keep passing at
 * exactly the moment it is needed: widen the mask until the addition wraps and
 * the highest id encodes to 0, which IS SERIAL_LOCK_FREE, and a held lock would
 * read as free. Assert the arithmetic before it can wrap, and assert the top of
 * the range separately rather than inferring it from id 0. */
_Static_assert(SERIAL_LOCK_OWNER_OF(0u) != SERIAL_LOCK_FREE,
               "serial lock: APIC id 0 must be distinguishable from a free lock");
_Static_assert((uint64_t)SERIAL_LOCK_ID_MASK + 1ULL <= 0xFFFFFFFFULL,
               "serial lock: owner encoding must fit the lock word without wrapping");
_Static_assert(SERIAL_LOCK_OWNER_OF(SERIAL_LOCK_ID_MASK) != SERIAL_LOCK_FREE,
               "serial lock: the highest APIC id must not encode to a free lock");

/* Emergency mode latch. One-way: set by serial_enter_emergency() on a terminal
 * path and never cleared, because nothing resumes after a panic. Once set, the
 * ordinary entry points re-route to the bounded non-blocking path, which makes
 * INDIRECT panic-path emitters safe against g_serial_lock without each one
 * opting in. It does NOT bound a lock taken ABOVE the serial layer -- klog_emit
 * holds s_klog_lock before reaching serial_write -- see serial.h SCOPE. */
static volatile uint32_t s_emergency = SERIAL_EMERG_OFF;

/* THE ALLOWANCES. One composite claim word per full-length wedged-UART wait:
 * SERIAL_CLAIM_VALID | generation | owner, or SERIAL_CLAIM_FREE.
 *
 * This array IS the budget. A claim is taken, and its owner and epoch recorded,
 * by a single compare-exchange on one of these words -- see the SERIAL_EMERG_RSVD
 * comment for why the allowance cannot live in the latch alongside an owner
 * recorded separately, in either order.
 *
 * The owner is the 8-bit initial APIC ID, the same CPUID-derived identity the
 * routing predicate uses, because the pre-arbitration dump must be
 * GS-INDEPENDENT: smp_this_cpu() falls back to &cpu_data[0] when GS is unset, so
 * an AP with no valid GS would record the BSP as owner and the BSP's refund
 * could then absorb the AP's charge. The field is exactly as wide as the id, so
 * no owner can fail to round-trip.
 *
 * The generation is what an epoch publication uses to invalidate every
 * outstanding claim AT ONCE: bumping it in the publishing compare-exchange makes
 * every claim carrying the old value stale, with no second pass over this array
 * and therefore no window in which some slots belong to the new epoch and others
 * still to the old one. That is strictly stronger than the explicit slot clear
 * it replaces, which had to happen inside the same word as the state.
 *
 * Not a lock, and nothing here can block a panic: every transition is one
 * bounded compare-exchange, and a CPU that loses a race degrades to a single
 * status probe rather than waiting.
 *
 * CACHELINE-SIZED for the same reason g_serial_lock is. All eight words fit in
 * one line by construction (asserted below), so a panicking CPU claiming an
 * allowance invalidates exactly one line, and never the line holding the lock or
 * the latch that every ordinary serial write reads per byte. */
#define SERIAL_CLAIM_FREE     0x00000000u
#define SERIAL_CLAIM_VALID    0x80000000u
#define SERIAL_CLAIM_OWNER    0x000000FFu
#define SERIAL_CLAIM_GEN      0x0000FF00u
#define SERIAL_CLAIM_GSHIFT   8u

#define serial_claim_gen(v)   (((v) & SERIAL_CLAIM_GEN) >> SERIAL_CLAIM_GSHIFT)
#define serial_claim_owner(v) ((v) & SERIAL_CLAIM_OWNER)
#define SERIAL_CLAIM_OF(gen, owner)                                            \
    (SERIAL_CLAIM_VALID |                                                      \
     ((((uint32_t)(gen)) & SERIAL_EMERG_GEN_MAX) << SERIAL_CLAIM_GSHIFT) |     \
     (((uint32_t)(owner)) & SERIAL_CLAIM_OWNER))

/* VALID is what keeps a claim in generation 0 by APIC id 0 -- the single most
 * likely claim on a uniprocessor boot -- from encoding to the same word that
 * means "free". The owner field must round-trip the whole id, and the claim's
 * generation field must be wide enough for the latch's, or a claim would go
 * stale (or fail to) on a generation the latch can still reach. */
_Static_assert(SERIAL_CLAIM_OF(0u, 0u) != SERIAL_CLAIM_FREE,
               "emergency claim: a live claim must never encode to FREE");
_Static_assert((SERIAL_CLAIM_VALID & (SERIAL_CLAIM_GEN | SERIAL_CLAIM_OWNER)) == 0u,
               "emergency claim: valid bit overlaps the generation or owner");
_Static_assert((SERIAL_CLAIM_GEN & SERIAL_CLAIM_OWNER) == 0u,
               "emergency claim: generation and owner fields overlap");
_Static_assert(SERIAL_CLAIM_OWNER == SERIAL_EMERG_CPU,
               "emergency claim: owner must mask exactly what the latch owner does");
_Static_assert((SERIAL_CLAIM_GEN >> SERIAL_CLAIM_GSHIFT) >= SERIAL_EMERG_GEN_MAX,
               "emergency claim: generation field narrower than the latch's");

static struct {
    volatile uint32_t slot[SERIAL_EMERG_STUCK_BYTES];
    uint8_t           _pad[SERIAL_CACHELINE -
                           (SERIAL_EMERG_STUCK_BYTES * sizeof(uint32_t))];
} __attribute__((aligned(SERIAL_CACHELINE))) s_emerg_claim_cl;

#define s_emerg_claim (s_emerg_claim_cl.slot)

/* One line, not merely aligned to one: a ninth allowance would split the array
 * across two lines and silently reintroduce the false sharing the padding
 * exists to remove, so raising the ceiling must fail the build here. */
_Static_assert(SERIAL_EMERG_STUCK_BYTES * sizeof(uint32_t) <= SERIAL_CACHELINE,
               "emergency claims must fit inside one cache line");

/* Test-only override of the LEDGER identity, so a single-CPU unit test can prove
 * that one CPU's refund cannot consume another CPU's charge -- the whole point of
 * the per-CPU ledger, and something no same-CPU test can distinguish from a
 * global counter.
 *
 * Deliberately scoped to the ledger ONLY. The owner claim in
 * serial_enter_emergency and the routing predicate both keep reading real CPUID,
 * because those decide whose crash evidence survives and must never be
 * influenced by test state. SERIAL_EMERG_NO_OWNER means "use the real id", which
 * is the value it holds in production for the life of the boot. */
static volatile uint32_t s_emerg_ledger_id_override = SERIAL_EMERG_NO_OWNER;

static uint32_t serial_emerg_ledger_id(void);

/* The owner is packed into the latch word above, NOT held separately, so that
 * state and owner publish in ONE atomic store and the owner can never be
 * mutated after publication. A REROUTED ordinary write from the owning CPU is
 * the panic's own evidence and must survive; one from any other CPU is a still
 * running thread (the stdout syscalls reach serial_putchar with user-controlled
 * bytes) and is discarded before it can take the lock or spend the budget.
 * SERIAL_EMERG_NO_OWNER means "nobody has armed yet", on which the routing
 * predicate fails OPEN and keeps every write. */

/* The terminal-epoch charge (timed-out waits paid for, plus reservations in
 * flight) is packed into the SAME word as the state and owner, so that
 * publishing an epoch and initializing its budget are ONE compare-exchange.
 * Held separately, the clear had to happen before the publishing CAS, and a
 * helper that lost the publication could still land its clear afterwards --
 * replenishing a live epoch by up to eight full waits per stale entrant. Packed,
 * a failed publication mutates nothing at all. */

/* Inline port I/O helpers */
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Panic-safe CPU identity, shared with the NMI depth counter (idt.c) and the
 * crash-evidence record (panic.c) so all of them provably mean the same CPU.
 * Full rationale -- why not smp_this_cpu(), why not per_cpu_data.lapic_id, and
 * the >255-CPU aliasing bound -- lives with the helper in cpu_security.h.
 *
 * The fault-freedom is required here, not merely nice: the routing predicate
 * runs on every rerouted write AFTER the epoch is armed, and an identity lookup
 * that could fault there would take a nested fault out of the panic owner, lose
 * owner arbitration, and abandon the remaining dump and BSOD. */
static inline uint32_t serial_emerg_self_cpu(void)
{
    return cpu_panic_safe_apic_id();
}

/* ---- g_serial_lock acquire / release ----
 *
 * Reimplemented against the owner-encoded word instead of calling
 * spin_lock_irqsave, because the entire point of that word is that TAKING the
 * lock and RECORDING the owner are one transition, and the generic primitive has
 * no owner to record. What is preserved verbatim is the IRQL discipline
 * (spinlock.c): save RFLAGS, cli, pack the previous KIRQL into bits 56-63 of the
 * saved flags, raise to at least DISPATCH_LEVEL -- because the callers of this
 * lock are the same mix of thread and IRQ context the generic primitive serves.
 *
 * The identity is derived ONCE, after `cli` and BEFORE the CAS loop. After cli
 * because a derivation that could be preempted mid-way might be attributed to
 * the wrong CPU; before the loop because cpu_panic_safe_apic_id is a serializing
 * CPUID -- a VM exit under KVM/WHPX -- and executing it inside the held region
 * makes every other CPU spin through it. GS is NOT consulted for identity: only
 * for the IRQL, exactly as the generic primitive already does. */
#define SERIAL_LOCK_IRQL_SHIFT  56u
#define SERIAL_LOCK_RFLAGS_MASK ((1ULL << SERIAL_LOCK_IRQL_SHIFT) - 1ULL)

static inline void serial_lock_acquire(uint64_t *flags)
{
    struct per_cpu_data *pcpu;
    KIRQL prev_irql;
    uint32_t me;

    *flags = local_irq_save();          /* save RFLAGS + cli atomically */
    me = SERIAL_LOCK_OWNER_OF(serial_emerg_self_cpu());

    /* Read per-CPU AFTER cli so nothing preempts between read and write. */
    pcpu = smp_this_cpu();
    prev_irql = pcpu->current_irql;
    *flags |= ((uint64_t)prev_irql) << SERIAL_LOCK_IRQL_SHIFT;
    if (pcpu->current_irql < DISPATCH_LEVEL)
        pcpu->current_irql = DISPATCH_LEVEL;

    /* TEST-and-test-and-set, not a bare CAS retry. A failed strong CAS still
     * takes the line for exclusive ownership, so retrying it back-to-back is a
     * `lock cmpxchg` storm that slows the HOLDER down -- and this holder is
     * uniquely expensive to slow, because serial_write keeps the lock across the
     * whole string and its UART THRE polling, with every waiter spinning
     * IRQ-disabled. So spin on a plain relaxed LOAD and only re-attempt the
     * locked CAS once the word actually reads free.
     *
     * `pause` is what makes the wait loop cheap: it hints the spin to the CPU,
     * avoids the memory-order-violation pipeline flush on exit, and drops SMT
     * and power pressure. barrier() alone emits NO instruction. The generic
     * spin_lock_irqsave (sched/spinlock.c) still has the bare-CAS shape; that is
     * worth fixing separately, but this lock is the one every klog line takes. */
    while (!serial_lock_try_acquire_owned(&g_serial_lock, me)) {
        while (__atomic_load_n(&g_serial_lock.owner, __ATOMIC_RELAXED) !=
               SERIAL_LOCK_FREE)
            __asm__ volatile ("pause");
    }
    barrier();      /* acquire fence: no hoisting of the critical section */
}

static inline void serial_lock_release(uint64_t flags)
{
    struct per_cpu_data *pcpu;
    KIRQL saved_irql;

    barrier();      /* release fence: all CS stores visible before the clear */
    /* ONE store frees the lock AND clears ownership -- there is no window in
     * which the word says "held by nobody". */
    __atomic_store_n(&g_serial_lock.owner, SERIAL_LOCK_FREE, __ATOMIC_RELEASE);

    saved_irql = (KIRQL)(flags >> SERIAL_LOCK_IRQL_SHIFT);
    flags &= SERIAL_LOCK_RFLAGS_MASK;   /* clear IRQL bits before RFLAGS restore */

    pcpu = smp_this_cpu();
    pcpu->current_irql = saved_irql;

    local_irq_restore(flags);           /* re-enables IRQs only if they were on */
}

/* The ownership POLICY itself, split out from the globals so it is testable
 * against a fixture lock -- no UART, no g_serial_lock, no boot state. The
 * branches that matter are the SUCCESSFUL acquisition and the SUCCESSFUL
 * handoff, and against the real global a single-CPU test cannot establish either
 * precondition without seizing the machine's actual serial lock.
 *
 * Both are a single compare-exchange, which IS the safety property in each
 * direction: an acquire only ever converts FREE to owned, so it can never
 * displace a live holder; a release only ever converts THIS owner to free, so a
 * caller whose id does not match leaves the word exactly as it was and can never
 * take the lock away from someone else.
 *
 * `owner` is the ENCODED value, SERIAL_LOCK_OWNER_OF(id), never a raw APIC id --
 * the parameter is named for what it is precisely because a raw id would be
 * accepted silently and would resurrect the hang (serial_emergency.h).
 *
 * SERIAL_LOCK_FREE is rejected rather than trusted. It cannot arise from
 * SERIAL_LOCK_OWNER_OF (the encoding is id + 1), so it only reaches here from a
 * caller that built an owner some other way -- and accepting it would let an
 * "acquire" store the free value, i.e. claim the lock and leave it unlocked. */
int serial_lock_try_acquire_owned(serial_lock_t *lock, uint32_t owner)
{
    uint32_t expected = SERIAL_LOCK_FREE;

    if (!lock || owner == SERIAL_LOCK_FREE)
        return 0;

    return __atomic_compare_exchange_n(&lock->owner, &expected, owner, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

int serial_lock_try_release_owned(serial_lock_t *lock, uint32_t owner)
{
    uint32_t cur = owner;

    if (!lock || owner == SERIAL_LOCK_FREE)
        return 0;

    return __atomic_compare_exchange_n(&lock->owner, &cur, SERIAL_LOCK_FREE, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* Hand g_serial_lock back if THIS CPU is the recorded holder.
 *
 * Called by a CPU that is about to park forever (the panic async-isolation
 * branch) so the surviving CPUs are not blocked on a lock whose owner will never
 * run again. Safe to call unconditionally: a CPU that owns nothing changes
 * nothing, so this can never steal a live holder's lock.
 *
 * The UART may be mid-character when this runs. That is accepted -- one garbled
 * line is strictly better than every later write blocking forever, and the
 * emergency writers restore a known line-control state before their own output
 * regardless (serial_emergency_restore_lcr). */
int serial_lock_release_if_owner_for(serial_lock_t *lock, uint32_t raw_id)
{
    return serial_lock_try_release_owned(lock, SERIAL_LOCK_OWNER_OF(raw_id));
}

void serial_lock_release_if_owner(void)
{
    /* The global and the identity are the ONLY things this adds over the
     * fixture-testable helper, so the raw-to-encoded conversion cannot drift
     * without a test seeing it. */
    (void)serial_lock_release_if_owner_for(&g_serial_lock,
                                           serial_emerg_self_cpu());
}

/* Forward declarations: the ordinary entry points below re-route into the
 * emergency path, which is defined further down beside the rest of it. */
static void serial_emergency_emit(const char *buf, uint32_t len,
                                  int terminal, uint32_t *recov);
static void serial_emergency_write_str(const char *str, int terminal, uint32_t ctx);
static int  serial_emerg_reroute_should_drop(void);

/* Raw unlocked UART write -- caller must hold g_serial_lock. Returns 0 if it
 * ABANDONED the byte because an epoch began arming, 1 if the byte was written.
 *
 * The wait is unbounded by design on a healthy system, but it aborts the moment
 * the latch leaves OFF (INIT counts). Two things depend on that: a CPU already
 * inside this loop when another begins arming must not keep spinning on a wedged
 * transmitter while holding g_serial_lock, and -- because the caller stops on a
 * 0 return -- its remaining ordinary output must not keep dribbling into the
 * crash record. Writing the byte anyway would put one more non-owner character
 * on the wire after the epoch began, which is exactly what the owner check
 * elsewhere exists to prevent. */
static inline int serial_putchar_raw(char c)
{
    if (!s_serial_port) return 1;

    /* Check BEFORE the wait, not only inside it. On a healthy UART THRE is
     * already set, so the loop body never runs -- and an in-loop-only check
     * would let an ordinary writer put its ENTIRE remaining string on the wire
     * during an armed epoch precisely when the transmitter is working. */
    if (serial_emerg_state(__atomic_load_n(&s_emergency, __ATOMIC_RELAXED))
        != SERIAL_EMERG_OFF)
        return 0;

    while ((inb(s_serial_port + UART_REG_LSR) & UART_LSR_THRE) == 0) {
        if (serial_emerg_state(__atomic_load_n(&s_emergency, __ATOMIC_RELAXED))
            != SERIAL_EMERG_OFF)
            return 0;          /* epoch arming -- do NOT write this byte */
        __asm__ volatile ("pause");
    }
    outb(s_serial_port + UART_REG_THR, (uint8_t)c);
    return 1;
}

void serial_init(void)
{
    uint16_t divisor;
    uint64_t flags;

    /* Read the serial port probed by the bootloader (S4/S10).
     * 0 = no UART detected; serial output becomes a no-op.
     * Non-zero overrides the COM1 default used for early klog before
     * serial_init runs.  Keep COM1 default if bootloader reports 0 --
     * early serial output needs a working port during the window between
     * kernel entry and serial_init(). */
    if (g_boot_info.serial_port)
        s_serial_port = g_boot_info.serial_port;

    if (!s_serial_port) return;

    /* Honor SPCR baud rate if available.
     * 0 = firmware-configured (SPCR baud code 0), preserve existing divisor.
     * Only accept known standard rates; anything else keeps existing divisor. */
    {
        uint32_t baud = g_boot_info.serial_baud;
        if (baud == 9600 || baud == 19200 || baud == 38400 ||
            baud == 57600 || baud == 115200) {
            divisor = (uint16_t)(UART_BASE_CLOCK / baud);
        } else if (baud == 0) {
            divisor = 0;  /* preserve firmware-configured divisor */
        } else {
            divisor = UART_DIV_38400;  /* unknown rate -- fall back to 38400 */
        }
    }

    /* Hold the lock across the WHOLE programming sequence, not just the writes.
     * The DLAB window below re-purposes the base port from the transmit holding
     * register to the divisor latch, so a concurrent writer landing inside it
     * would send its bytes to the divisor and corrupt the baud rate -- a strictly
     * worse outcome than the byte interleaving an unlocked THR write causes.
     * This closes the cross-CPU half of that race; the same-CPU half (an abort
     * re-entering here) is handled by the LCR re-assert in the emergency path,
     * which cannot be solved with a lock.
     *
     * ORDERING: serial_lock_acquire dereferences per-CPU data (smp_this_cpu()->
     * current_irql) without a NULL check, so serial_init MUST run after
     * smp_early_bsp_init(). It does -- boot_hw.c calls them four lines apart --
     * and this is not a new constraint: serial_write has always taken the same
     * lock, and the IRQL half of that acquire is a verbatim copy of what
     * spin_lock_irqsave did before this lock grew its own owner word, so any
     * pre-per-CPU serial output would already have faulted. Note the ordering
     * binds the IRQL read ONLY -- the owner identity comes from CPUID and needs
     * no per-CPU data at all. */
    serial_lock_acquire(&flags);

    outb(s_serial_port + UART_REG_IER, 0x00);    /* Disable interrupts */

    if (divisor > 0) {
        outb(s_serial_port + UART_REG_LCR, UART_LCR_DLAB);
        outb(s_serial_port + UART_REG_DLL, (uint8_t)(divisor & 0xFF));
        outb(s_serial_port + UART_REG_DLH, (uint8_t)((divisor >> 8) & 0xFF));
    }
    outb(s_serial_port + UART_REG_LCR, UART_LCR_8N1);  /* 8N1 (also clears DLAB) */
    outb(s_serial_port + UART_REG_FCR, UART_FCR_INIT);
    outb(s_serial_port + UART_REG_MCR, UART_MCR_INIT);

    serial_lock_release(flags);
}

void serial_putchar(char c)
{
    uint64_t flags;

    if (serial_in_emergency()) {
        /* REROUTED, not direct: drop on contention rather than writing
         * unlocked, so a CPU still running user code cannot interleave stdout
         * bytes into the panic record. See serial.h. */
        uint32_t recov = 0;
        /* Positively-identified non-owner: discard BEFORE touching the lock or
         * the budget. Dropping only on contention was not enough -- with the
         * lock free, a CPU still running user code would acquire it, emit
         * user-controlled bytes into the crash record, and spend terminal
         * reservations the panic owner then would not have. */
        if (serial_emerg_reroute_should_drop())
            return;
        serial_emergency_emit(&c, 1u, 1 /*terminal*/, &recov);
        return;
    }

    serial_lock_acquire(&flags);

    /* RECHECK under the lock. The test above and this acquisition are two
     * operations, and the wait between them is unbounded under contention -- so
     * a CPU can read OFF, block on the lock, and wake up inside an armed epoch,
     * then perform UNBOUNDED raw writes with none of the emergency bounds and
     * none of the non-owner drop. Recheck and re-route instead. */
    if (serial_in_emergency()) {
        serial_lock_release(flags);
        serial_putchar(c);
        return;
    }

    if (!serial_putchar_raw(c)) {
        /* An epoch began arming mid-wait. Hand the byte to the owner-aware
         * emergency route DIRECTLY -- never by re-calling serial_putchar, which
         * during the INIT window still reads the latch as not-armed, retakes
         * this same lock, abandons again, and recurses without bound on a panic
         * stack. */
        uint32_t recov = 0;
        serial_lock_release(flags);
        if (!serial_emerg_reroute_should_drop())
            serial_emergency_emit(&c, 1u, 1 /*terminal*/, &recov);
        return;
    }
    serial_lock_release(flags);
}

/* Hold the lock for the entire string so no other caller can interleave */
void serial_write(const char *str)
{
    uint64_t flags;

    if (serial_in_emergency()) {
        /* See serial_putchar: a positively-identified non-owner discards here,
         * before any lock acquisition or budget reservation. */
        if (serial_emerg_reroute_should_drop())
            return;
        /* PANIC_CTX_UNKNOWN: this is the REROUTED ordinary path, reached from
         * serial_write / serial_putchar once the latch is armed. The indirect
         * emitters funnelled through here (klog's serial sink, subsystem dumps)
         * cannot say which vector is being handled above them, so they keep the
         * plain load they have always used rather than being handed the NMI
         * hazard by default. */
        serial_emergency_write_str(str, 1 /*terminal*/, PANIC_CTX_UNKNOWN);
        return;
    }

    serial_lock_acquire(&flags);

    /* RECHECK under the lock -- see serial_putchar. A residual remains: the
     * epoch can be armed AFTER this check, while the loop below is already
     * running, and that write is still unbounded. It cannot silence the panic
     * though: the owner's emergency writers try-lock, fail against this holder,
     * and proceed unlocked, so the crash record still reaches the wire. */
    if (serial_in_emergency()) {
        serial_lock_release(flags);
        serial_write(str);
        return;
    }

    while (*str) {
        if (*str == '\n' && !serial_putchar_raw('\r'))
            break;
        if (!serial_putchar_raw(*str))
            break;              /* epoch arming -- stop, re-route the remainder */
        str++;
    }

    if (*str) {
        /* Abandoned mid-string. Release, then emit the remainder through the
         * emergency route DIRECTLY -- re-calling serial_write would re-enter the
         * blocking path during the INIT window and recurse without bound. */
        serial_lock_release(flags);
        /* PANIC_CTX_UNKNOWN: this is the REROUTED ordinary path, reached from
         * serial_write / serial_putchar once the latch is armed. The indirect
         * emitters funnelled through here (klog's serial sink, subsystem dumps)
         * cannot say which vector is being handled above them, so they keep the
         * plain load they have always used rather than being handed the NMI
         * hazard by default. */
        if (!serial_emerg_reroute_should_drop()) {
            serial_emergency_write_str(str, 1 /*terminal*/, PANIC_CTX_UNKNOWN);
        }
        return;
    }
    serial_lock_release(flags);
}

char serial_trygetchar(void)
{
    /* Check Line Status Register bit 0 (Data Ready) */
    if (!s_serial_port) return 0;
    if ((inb(s_serial_port + UART_REG_LSR) & UART_LSR_DR) == 0)
        return 0;
    return (char)inb(s_serial_port + UART_REG_RBR);
}

/* ---------------------------------------------------------------------------
 * Emergency (abort-safe) path -- see serial.h for the contract.
 * ------------------------------------------------------------------------- */

void serial_enter_emergency(void)
{
    uint32_t me = serial_emerg_self_cpu() & SERIAL_EMERG_CPU;

    /* STATE-DRIVEN RETRY LOOP. Every transition must be decided from a FRESHLY
     * observed state, because the budget field lives in this same word and a
     * concurrent reserve/return changes the word without changing the state.
     * Two bugs come from assuming otherwise, and both were live:
     *
     *   - a failed OFF->INIT compare-exchange does NOT imply somebody else
     *     armed; it may simply be a budget charge. Falling through and
     *     publishing from that word would derive the owner from an OFF word,
     *     whose owner bits are zero -- recording CPU 0, so a panic on any other
     *     APIC ID would then discard its own rerouted evidence as a non-owner.
     *   - a budget change between observing INIT and publishing makes the
     *     publishing compare-exchange fail. Returning there would leave the
     *     latch stuck in INIT forever, serial_in_emergency() false, and every
     *     terminal caller back on the ordinary blocking path.
     *
     * So: never derive an owner from an OFF word, retry publication after a
     * budget-only change, and leave only on an observed ARMED. */
    for (;;) {
        uint32_t cur = __atomic_load_n(&s_emergency, __ATOMIC_ACQUIRE);

        if (serial_emerg_state(cur) == SERIAL_EMERG_ARMED)
            return;                     /* epoch published -- nothing to do */

        if (serial_emerg_state(cur) == SERIAL_EMERG_OFF) {
            /* The OWNER is chosen here and NOWHERE else, which is what makes it
             * immutable for the epoch: it is packed with the state, so no later
             * entrant can rewrite it and demote the panicking CPU. */
            /* PRESERVE THE GENERATION, not just the slots. Dropping it here
             * rewound epoch identity to 0, so the publishing bump below
             * republished a generation that had already been used -- and a token
             * minted under it, and deliberately invalidated since, became valid
             * again and could return a charge belonging to the live panic. The
             * generation must only ever move forward. */
            uint32_t want = serial_emerg_claim_word(cur, me);
            if (!__atomic_compare_exchange_n(&s_emergency, &cur, want, 0,
                                             __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                continue;               /* re-observe: state may not have moved */
            cur = want;
        }

        /* `cur` is INIT|owner(|slots|gen) -- ours, or one this CPU is HELPING to
         * finish. Helping is required, not optional: `cli` does not mask NMI or
         * #MC, so an initializer can be interrupted between claiming INIT and
         * publishing, and that nested event is itself terminal so it never
         * returns. Without a helper the latch would stay INIT forever. There
         * must be NO terminal path that returns while the latch is INIT.
         *
         * A helper completes the epoch WITHOUT adopting it -- the owner bits in
         * `cur` are preserved -- and the SAME compare-exchange clears every slot
         * and advances the generation, so publication and epoch-start are
         * indivisible and a losing entrant mutates nothing at all. */
        {
            /* The generation advances in this SAME compare-exchange, for the
             * same reason the budget clear lives here: publication, epoch start
             * and epoch identity must be one indivisible act. A generation
             * bumped separately could be observed by a reserve that had already
             * read the old value, minting a token for an epoch that no longer
             * exists. */
            uint32_t armed = serial_emerg_publish_word(cur);
            if (__atomic_compare_exchange_n(&s_emergency, &cur, armed, 0,
                                            __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
                return;
            /* Budget moved under us -- retry from a fresh observation. */
        }
    }
}

/* PURE latch-transition arithmetic, factored out so the transitions can be
 * tested without arming the one-way latch (which would degrade serial output for
 * the rest of the boot). serial_enter_emergency below uses exactly these, so a
 * test of them is a test of it.
 *
 * The OFF->INIT case is the one that most needs pinning: an earlier revision
 * dropped the generation here, rewinding epoch identity so a token that had been
 * deliberately invalidated became valid again and could return a charge
 * belonging to the live panic. Nothing in the reset-based tests can reach that
 * transition, so without this helper the regression had no test at all. */
uint32_t serial_emerg_claim_word(uint32_t cur, uint32_t owner)
{
    return SERIAL_EMERG_INIT | (owner & SERIAL_EMERG_CPU) |
           (cur & SERIAL_EMERG_GEN);
}

/* Publication advances the generation, and THAT is what starts the new
 * accounting epoch: every claim in s_emerg_claim[] carrying the old generation
 * becomes stale in the same compare-exchange that publishes ARMED. The previous
 * shape cleared an explicit slot bitmap in this word instead, which worked only
 * because the bitmap lived in the word being published; with the allowances in
 * their own array a separate clearing pass would leave a window where some
 * allowances belonged to the new epoch and others still to the old one. The
 * generation bump has no such window and needs no pass. */
uint32_t serial_emerg_publish_word(uint32_t cur)
{
    uint32_t nextgen = (serial_emerg_gen(cur) + 1u) & SERIAL_EMERG_GEN_MAX;

    return SERIAL_EMERG_ARMED | serial_emerg_cpu(cur) |
           (nextgen << SERIAL_EMERG_GSHIFT);
}

int serial_emerg_ctx_allows_guarded_read(uint32_t ctx)
{
    /* OPT-IN, and pure so it can be tested without a UART.
     *
     * Written as an equality against the one permitted value rather than an
     * inequality against the forbidden one, because the failure directions are
     * not symmetric: `ctx != PANIC_CTX_NMI` would silently route every
     * UNKNOWN caller (and any value added later) through the fault-returning
     * read, handing them the nested-NMI IST2 hazard, and no existing test would
     * notice. This way an unrecognized context degrades to the plain load. */
    return ctx == PANIC_CTX_NORMAL;
}

int serial_emerg_should_drop_for(uint32_t writer, uint32_t owner)
{
    if (writer == SERIAL_EMERG_NO_OWNER || owner == SERIAL_EMERG_NO_OWNER)
        return 0;                       /* fail open -- keep the evidence */
    return writer != owner;
}

/* Same predicate with the writer identity SUPPLIED rather than recomputed.
 *
 * The identity comes from CPUID, which is serializing and traps to the
 * hypervisor under KVM/WHPX/Hyper-V. It is also loop-invariant: a CPU does not
 * change its own APIC id mid-record. So any caller re-testing ownership per byte
 * hoists it once and re-reads only the latch, which is the part that can change
 * under it. */
static int serial_emerg_drop_for_writer(uint32_t me)
{
    uint32_t cur = __atomic_load_n(&s_emergency, __ATOMIC_ACQUIRE);

    if (serial_emerg_state(cur) == SERIAL_EMERG_OFF)
        return 0;                       /* fail open -- nobody has claimed */
    return serial_emerg_should_drop_for(me, serial_emerg_cpu(cur));
}

static int serial_emerg_reroute_should_drop(void)
{
    uint32_t cur = __atomic_load_n(&s_emergency, __ATOMIC_ACQUIRE);

    /* Any non-OFF state carries the claimant's id -- INIT included. Treating
     * INIT as ownerless would fail open for the whole arming window and let
     * non-owner writes back into the crash record exactly while it is being
     * established. Only a genuinely unclaimed latch fails open. */
    if (serial_emerg_state(cur) == SERIAL_EMERG_OFF)
        return 0;                       /* fail open -- nobody has claimed */
    return serial_emerg_should_drop_for(
        serial_emerg_self_cpu() & SERIAL_EMERG_CPU, serial_emerg_cpu(cur));
}

int serial_in_emergency(void)
{
    return serial_emerg_state(__atomic_load_n(&s_emergency, __ATOMIC_ACQUIRE))
           == SERIAL_EMERG_ARMED;
}

int serial_emergency_acquire(serial_lock_t *lock, uint32_t owner)
{
    return serial_lock_try_acquire_owned(lock, owner);
}

/* A plain release-store, not an owner-compare: `acquired` is only ever non-zero
 * on the return path of an acquire that WON, so this caller provably owns the
 * word. It touches no IRQL, because the try-acquire raised none. */
void serial_emergency_release(serial_lock_t *lock, int acquired)
{
    if (lock && acquired)
        __atomic_store_n(&lock->owner, SERIAL_LOCK_FREE, __ATOMIC_RELEASE);
}

/* Force a known line-control state before emergency output.
 *
 * serial_init() re-purposes the base port to the divisor latch across its DLAB
 * window, and a lock cannot protect the SAME-CPU case: an abort taken inside
 * that window re-enters here, finds the lock held by the interrupted code (or,
 * before the lock was added, free), and would send the panic header to the
 * divisor latch instead of the transmitter -- corrupting the baud rate AND
 * emitting nothing. Re-asserting 8N1 clears DLAB and restores the format
 * serial_init configures anyway, so this recovers the port rather than imposing
 * a new configuration on it.
 *
 * Residual: if the abort landed BETWEEN the two divisor-latch writes the
 * divisor is half-programmed and the baud rate is wrong, which garbles output.
 * That cannot be repaired without re-deriving the divisor from boot_info, which
 * is more fallible work than a terminal path should do; the window is confined
 * to Phase 0 serial_init and is a few port writes wide. */
static inline void serial_emergency_restore_lcr(void)
{
    if (!s_serial_port) return;
    outb(s_serial_port + UART_REG_LCR, UART_LCR_8N1);
}

/* Claim one of the SERIAL_EMERG_STUCK_BYTES full-wait allowances.
 * Returns a nonzero TOKEN if this call may spin, or SERIAL_EMERG_NO_TOKEN if it
 * must take the single-probe path. The token names the slot claimed and the
 * epoch it was claimed in; hand it back to serial_emerg_return exactly once.
 *
 * STRONG CAS with a bounded attempt count. Strong so a spurious failure cannot
 * cost an allowance; bounded because this runs INSIDE serial_emergency_emit's
 * locked region, so an unbounded retry under contention would hold
 * g_serial_lock while looping -- the opposite of the never-block contract.
 * A caller that loses the race simply probes once, which is the correct
 * degradation: it means other CPUs are already spending the budget. */
/* Token layout: VALID | generation | slot index. The slot is what makes a return
 * name the exact charge it owns; the generation is what stops it naming a charge
 * in an epoch that has since been replaced. */
#define SERIAL_EMERG_TOK_SLOT   0x000000FFu
#define SERIAL_EMERG_TOK_GSHIFT 8u

/* The identity a claim records. Test-overridable so one CPU can drive both sides
 * of an ownership property that a same-CPU fixture could not otherwise tell from
 * a global counter. */
static uint32_t serial_emerg_ledger_id(void)
{
    uint32_t o = __atomic_load_n(&s_emerg_ledger_id_override, __ATOMIC_RELAXED);

    if (o != SERIAL_EMERG_NO_OWNER)
        return o & SERIAL_EMERG_CPU;
    return serial_emerg_self_cpu() & SERIAL_EMERG_CPU;
}

void serial_emerg_set_ledger_id_for_test(uint32_t id)
{
    __atomic_store_n(&s_emerg_ledger_id_override, id, __ATOMIC_RELAXED);
}

/* The live accounting epoch. Exposed because a granted token carries the epoch
 * it was claimed under, and "the token never names a dead epoch" is otherwise
 * unassertable: the generation and the claim live in different words, so the
 * only way to check that reserve rejected a claim stranded by a publication is
 * to compare the token's epoch against the current one. */
uint32_t serial_emerg_gen_for_test(void)
{
    return serial_emerg_gen(__atomic_load_n(&s_emergency, __ATOMIC_RELAXED));
}

/* THE SINGLE TRANSITION. Take allowance `idx` for {gen, owner}, but only if it
 * still reads exactly `expect` -- which the caller observed to be either FREE or
 * a claim from a dead epoch.
 *
 * Deliberately four lines around one __atomic_compare_exchange_n, the same shape
 * as serial_lock_try_acquire_owned and for the same reason: the property that
 * taking an allowance and recording its owner are ONE lock-prefixed instruction
 * is not observable from any post-state a test can assert, because a two-step
 * implementation reaches an identical post-state. It is checked instead by
 * disassembling this function in the built object -- tools/atomic-claim-check.
 *
 * External linkage for that reason as much as for the unit test: a static helper
 * is inlined into its caller, and the property then has no symbol to check and
 * no way to distinguish this compare-exchange from the caller's other atomics. */
int serial_emerg_claim_slot(uint32_t idx, uint32_t expect,
                            uint32_t gen, uint32_t owner)
{
    return __atomic_compare_exchange_n(&s_emerg_claim[idx], &expect,
                                       SERIAL_CLAIM_OF(gen, owner), 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

/* Release allowance `idx`, but only while it still reads exactly `expect`.
 *
 * EXACT rather than "clear if mine": a claim whose generation has since been
 * replaced belongs to the new epoch's holder, and an unconditional store would
 * erase that CPU's live charge. The exact compare is what makes a release
 * incapable of touching anything but the claim the caller actually holds. */
int serial_emerg_release_slot(uint32_t idx, uint32_t expect)
{
    return __atomic_compare_exchange_n(&s_emerg_claim[idx], &expect,
                                       SERIAL_CLAIM_FREE, 0,
                                       __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

/* A claim is LIVE when it is valid and carries the current generation. Anything
 * else -- free, or minted under an epoch that has since been republished -- is
 * available, and is reclaimed by exact compare-exchange against the stale word
 * so that reclaiming can never race a live holder into oblivion. */
static int serial_emerg_claim_is_live(uint32_t c, uint32_t gen)
{
    return (c & SERIAL_CLAIM_VALID) && serial_claim_gen(c) == gen;
}

uint32_t serial_emerg_reserve(void)
{
    uint32_t attempts, idx, gen, owner, c;

    owner = serial_emerg_ledger_id();

    for (attempts = 0; attempts < SERIAL_EMERG_CAS_TRIES; attempts++) {
        /* Read the epoch ONCE per sweep and bind the whole attempt to it. A
         * generation re-read per slot could claim under an epoch that had
         * already been replaced by the time the CAS landed. */
        gen = serial_emerg_gen(__atomic_load_n(&s_emergency, __ATOMIC_RELAXED));

        /* Lowest available allowance, scanning ALL of them rather than stopping
         * at the first. Stopping was the flaw in the claim-then-publish design
         * this replaces: an allowance that could not be taken made every later
         * one unreachable, so the budget could read as exhausted with most of it
         * idle. Here a slot is skipped only because it is genuinely charged. */
        for (idx = 0; idx < SERIAL_EMERG_STUCK_BYTES; idx++) {
            c = __atomic_load_n(&s_emerg_claim[idx], __ATOMIC_RELAXED);
            if (serial_emerg_claim_is_live(c, gen))
                continue;                   /* charged in the live epoch */

            /* ONE transition: this compare-exchange takes the allowance AND
             * records who owns it and when. There is no second step, so there is
             * no instant at which a charge exists without an owner -- which is
             * the entire reason the allowance does not live in the latch word.
             *
             * `c` is passed as the expected value rather than FREE so that a
             * dead-epoch claim is reclaimed by the same instruction, and so that
             * losing the race to another CPU (or to a nested abort on this one)
             * fails the CAS instead of overwriting whatever landed. */
            if (serial_emerg_claim_slot(idx, c, gen, owner)) {
                /* RE-READ THE EPOCH AFTER THE CLAIM LANDS.
                 *
                 * The generation is read before the compare-exchange and the
                 * two live in different words, so a publication can land
                 * between them -- and the claim is then installed carrying an
                 * epoch that no longer exists. Nothing counts such a claim
                 * (waits and charges_self both require the live generation) and
                 * the next reserve reclaims its slot as stale, yet the caller
                 * has still been told it may spend a full-length wait. That is
                 * an allowance granted outside the ceiling: a stall the panic
                 * path's budget never accounted for.
                 *
                 * This is a REGRESSION AGAINST THE SHAPE IT REPLACED, which is
                 * why the check is here rather than argued away. When the
                 * allowance was a bit in the latch, the reservation
                 * compare-exchange targeted the same word the publication did,
                 * so a publication in that window simply failed the CAS. Moving
                 * the allowance into its own word removed that coupling; this
                 * restores it, by releasing a claim that turns out to be stale
                 * and re-sweeping under the new epoch.
                 *
                 * Parity and no more. A publication landing AFTER this point
                 * invalidates a reservation that was legitimately admitted
                 * while its epoch was live, and that caller finishes its wait --
                 * exactly as it did before, and deliberately: the alternative is
                 * re-reading shared state on every iteration of the bounded spin
                 * loop, on the panic path, to shorten a stall that is already
                 * bounded. */
                if (serial_emerg_gen(__atomic_load_n(&s_emergency,
                                                     __ATOMIC_RELAXED)) != gen) {
                    (void)serial_emerg_release_slot(idx,
                                                    SERIAL_CLAIM_OF(gen, owner));
                    break;                  /* re-sweep under the new epoch */
                }
                return SERIAL_EMERG_TOKEN_VALID |
                       (gen << SERIAL_EMERG_TOK_GSHIFT) | idx;
            }

            /* Lost this one; the sweep continues to the next allowance rather
             * than retrying the same index, so N contending CPUs take N
             * different slots instead of serializing on the lowest. */
        }

        /* A full sweep found every allowance live. Re-sweep only if the epoch
         * moved under us -- a republication frees all of them at once, and
         * spinning against a genuinely saturated budget is exactly the blocking
         * this path must never do. */
        if (serial_emerg_gen(__atomic_load_n(&s_emergency, __ATOMIC_RELAXED)) == gen)
            return SERIAL_EMERG_NO_TOKEN;   /* every allowance is outstanding */
    }
    return SERIAL_EMERG_NO_TOKEN;
}

void serial_emerg_return(uint32_t token)
{
    uint32_t gen, idx, c;

    /* EPOCH-VALIDATED, AND THE TOKEN IS SINGLE-USE.
     *
     * Validated: a token minted under generation G clears a slot only while the
     * latch still reads G. That closes the stale-return hole -- a reservation
     * taken before an epoch was published and handed back after it used to
     * credit the NEW epoch, granting up to a full extra ceiling of full-length
     * UART waits beyond the two-phase bound.
     *
     * SINGLE-USE, deliberately NOT idempotent, and the distinction is worth
     * stating because the earlier draft of this code claimed idempotency and was
     * wrong. A token carries a generation and a slot INDEX, so two reservations
     * of the same slot in the same generation are byte-identical: if T1 is
     * returned, reserve() hands the freed slot straight back out as T2, and a
     * second return of T1 would then clear T2's live charge and erase a timeout
     * that really happened. No validation here can separate them -- telling
     * incarnations apart needs a per-slot counter, and the latch word has six
     * free bits against the sixteen that would take.
     *
     * So the contract is that a caller returns each token exactly once, which is
     * what every caller in the tree does: serial_putchar_raw_bounded reserves
     * and returns one local token per byte, and serial_emerg_refund_self works
     * from the claims themselves rather than from tokens. The checks below are
     * therefore defensive against a malformed or stale token, not a licence to
     * return the same one twice. */
    if (!(token & SERIAL_EMERG_TOKEN_VALID))
        return;
    gen = (token >> SERIAL_EMERG_TOK_GSHIFT) & SERIAL_EMERG_GEN_MAX;

    /* VALIDATE THE INDEX BEFORE INDEXING WITH IT. The token's slot field is 8
     * bits, so a malformed token can carry 0..255 against an array of
     * SERIAL_EMERG_STUCK_BYTES, and the read that follows is the one place a
     * bad token could reach memory that is not an allowance at all. The bound
     * is the array's own extent, so raising the ceiling cannot leave this
     * check behind. */
    idx = token & SERIAL_EMERG_TOK_SLOT;
    if (idx >= SERIAL_EMERG_STUCK_BYTES)
        return;                             /* malformed token: no such slot */

    /* ONE exact release, no loop. The claim word can only be changed by this
     * CPU (its owner) or by a reclaim that requires the generation to have
     * moved, and the exact compare rejects the latter, so there is no contention
     * to retry against -- the previous shape needed a bounded CAS loop only
     * because it was mutating a word every other panicking CPU also wrote. */
    c = __atomic_load_n(&s_emerg_claim[idx], __ATOMIC_RELAXED);
    if (!serial_emerg_claim_is_live(c, gen))
        return;                             /* dead epoch, or already returned */

    /* OWNERSHIP IS CHECKED, not assumed. Every in-tree caller returns on the CPU
     * that reserved -- serial_putchar_raw_bounded does both inside one
     * interrupt-disabled region -- so this rejects only a token that has
     * genuinely crossed CPUs, which would otherwise release a charge attributed
     * to somebody else and put the ceiling back out by one wait. That is the
     * exact failure the composite claim exists to make impossible, so it is
     * enforced here rather than documented as a caller obligation. */
    if (serial_claim_owner(c) != serial_emerg_ledger_id())
        return;

    (void)serial_emerg_release_slot(idx, c);
}

uint32_t serial_emerg_charges_self(void)
{
    uint32_t gen = serial_emerg_gen(__atomic_load_n(&s_emergency, __ATOMIC_RELAXED));
    uint32_t me  = serial_emerg_ledger_id();
    uint32_t idx, n = 0;

    /* Read straight off the claims. This used to be a popcount of a separate
     * per-CPU ledger that reserve maintained alongside the global bitmap, and
     * the two could disagree whenever an abort landed between them; there is now
     * one source, so the count cannot be a reconstruction that is wrong. */
    for (idx = 0; idx < SERIAL_EMERG_STUCK_BYTES; idx++) {
        uint32_t c = __atomic_load_n(&s_emerg_claim[idx], __ATOMIC_RELAXED);

        if (serial_emerg_claim_is_live(c, gen) && serial_claim_owner(c) == me)
            n++;
    }
    return n;
}

/* Hand back up to `n` charges that THIS CPU is recorded as holding.
 *
 * This is the attributable refund the async-isolation branch needs. Bounding it
 * by this CPU's own slot -- rather than by a delta between two reads of the
 * shared budget -- is the whole point: a delta cannot tell this CPU's charge
 * from one another CPU took in the same epoch, so a concurrent writer's charge
 * could be absorbed by a refund that never made it. Here a CPU can only ever
 * give back what it is itself recorded as holding, in the epoch it holds it. */
void serial_emerg_refund_self(uint32_t n)
{
    uint32_t gen = serial_emerg_gen(__atomic_load_n(&s_emergency, __ATOMIC_RELAXED));
    uint32_t me  = serial_emerg_ledger_id();
    uint32_t idx;

    /* Release exactly n of THIS CPU's own allowances, lowest first. Anything not
     * selected stays charged, which is the invariant the async refund depends
     * on: it hands back only what its own dump spent and must leave this CPU's
     * earlier timeouts standing.
     *
     * One exact compare-exchange per allowance, and NONE of them can fail
     * spuriously or under contention: an allowance owned by this CPU in the live
     * generation is written by nobody else, and a reclaim by another CPU
     * requires the generation to have moved -- which the exact compare rejects
     * on its own. The previous shape needed a bounded retry loop over a shared
     * bitmap word and could exhaust it, leaving charges outstanding that the
     * next panic would then find already spent; that residual is gone with the
     * shared word, not merely narrowed.
     *
     * A release that fails therefore means precisely one thing: the epoch was
     * republished under us, so the charge no longer exists and nothing is owed.
     * Stopping on it rather than continuing is correct for the same reason --
     * every remaining selection belongs to the same dead epoch. */
    for (idx = 0; n && idx < SERIAL_EMERG_STUCK_BYTES; idx++) {
        uint32_t c = __atomic_load_n(&s_emerg_claim[idx], __ATOMIC_RELAXED);

        if (!serial_emerg_claim_is_live(c, gen) || serial_claim_owner(c) != me)
            continue;
        if (!serial_emerg_release_slot(idx, c))
            return;                      /* epoch already ended: nothing owed */
        n--;
    }
}

uint32_t serial_emerg_waits(void)
{
    uint32_t gen = serial_emerg_gen(__atomic_load_n(&s_emergency, __ATOMIC_RELAXED));
    uint32_t idx, n = 0;

    for (idx = 0; idx < SERIAL_EMERG_STUCK_BYTES; idx++)
        if (serial_emerg_claim_is_live(
                __atomic_load_n(&s_emerg_claim[idx], __ATOMIC_RELAXED), gen))
            n++;
    return n;
}

void serial_emerg_reset_for_test(void)
{
    uint32_t attempts, cur, want, nextgen;

    /* Advancing the generation STARTS A NEW ACCOUNTING EPOCH, exactly as the
     * publishing CAS does, and that single bump is what releases every
     * outstanding allowance: each claim still carrying the old generation is
     * stale from this instruction onward, reclaimable by the next reserve, and
     * counted by nothing. It must also advance for the token's sake -- otherwise
     * a token minted before the reset stays valid and its return releases a
     * charge in the fresh epoch, which is the very hole the generation exists to
     * close, reintroduced through the test hook. Bumping here is what lets a
     * test mint a token, reset, and assert the stale return is ignored.
     *
     * The claim array is deliberately NOT cleared: writing it would be a second
     * step with a window, and a stale claim is already indistinguishable from a
     * free one to every reader. */
    for (attempts = 0; attempts < SERIAL_EMERG_CAS_TRIES; attempts++) {
        cur     = __atomic_load_n(&s_emergency, __ATOMIC_RELAXED);
        nextgen = (serial_emerg_gen(cur) + 1u) & SERIAL_EMERG_GEN_MAX;
        want    = (cur & ~SERIAL_EMERG_GEN) | (nextgen << SERIAL_EMERG_GSHIFT);
        if (__atomic_compare_exchange_n(&s_emergency, &cur, want, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return;
    }
}

/* Bounded raw write. Returns 1 if the transmitter drained, 0 if it did not.
 * On timeout the byte is written ANYWAY: a stuck LSR status bit does not always
 * mean a dead transmitter, and a byte that might reach the wire beats a byte
 * that certainly does not.
 *
 * `terminal` selects which budget pays: the shared monotonic epoch budget for
 * an armed panic, or the caller's own per-call counter for a recoverable
 * report. `recov` is that counter and is unused when `terminal` is set.
 *
 * The budget lives HERE rather than in the callers because the panic path
 * reaches this function through both of them -- whole strings via
 * serial_write_emergency and single characters via serial_putchar_emergency
 * (panic.c's hex writer) -- so a caller-side counter cannot bound the dump. */
static int serial_putchar_raw_bounded(char c, int terminal, uint32_t *recov)
{
    uint32_t spins;
    uint32_t token = SERIAL_EMERG_NO_TOKEN;
    int      may_wait;

    if (!s_serial_port) return 0;

    /* PROBE BEFORE RESERVING. A byte that finds the transmitter already ready
     * never waits, and the budget counts WAITS -- so reserving for it was both
     * wasted work and a misreading of what the allowance is for. The old order
     * charged and immediately refunded a slot for every healthy byte, which on a
     * working UART is the overwhelmingly common case: two CAS loops per byte on
     * a word every other panicking CPU is also touching, plus the ledger update,
     * for an accounting result of exactly zero.
     *
     * Observable behaviour is unchanged: the byte goes out either way, and the
     * budget ends where it did. What changes is that a healthy transmitter no
     * longer pays the contended reservation protocol at all. */
    if ((inb(s_serial_port + UART_REG_LSR) & UART_LSR_THRE) != 0) {
        outb(s_serial_port + UART_REG_THR, (uint8_t)c);
        return 1;
    }

    /* Not ready: this byte is about to WAIT, which is what the budget bounds. */
    if (terminal) {
        token    = serial_emerg_reserve();
        may_wait = (token != SERIAL_EMERG_NO_TOKEN);
    } else {
        may_wait = (*recov < SERIAL_RECOV_STUCK_BYTES);
    }

    /* No allowance (budget spent, or lost the reservation race): ONE status
     * read, no spin. The byte goes out if the transmitter happens to be ready,
     * otherwise it is dropped rather than paid for. */
    if (!may_wait) {
        if ((inb(s_serial_port + UART_REG_LSR) & UART_LSR_THRE) == 0)
            return 0;
        outb(s_serial_port + UART_REG_THR, (uint8_t)c);
        return 1;
    }

    spins = terminal ? SERIAL_EMERG_THRE_SPINS : SERIAL_RECOV_THRE_SPINS;
    while ((inb(s_serial_port + UART_REG_LSR) & UART_LSR_THRE) == 0) {
        if (--spins == 0) {
            /* Timed out. The reservation IS the charge and is NOT returned --
             * that is what makes the terminal budget monotonic. */
            outb(s_serial_port + UART_REG_THR, (uint8_t)c);
            if (!terminal)
                (*recov)++;
            return 0;
        }
        __asm__ volatile ("pause");
    }

    outb(s_serial_port + UART_REG_THR, (uint8_t)c);
    if (terminal)
        serial_emerg_return(token);  /* drained -- hand back only OUR reservation,
                                      * and only while its epoch is still live */
    return 1;
}

/* Emit an already-captured chunk. Takes and releases the lock itself, and
 * touches no caller-supplied pointer, so nothing inside the locked region can
 * fault on memory this driver does not own.
 *
 * Always proceeds on a failed try-lock -- evidence at all costs. Output that
 * must NOT reach the wire (a rerouted write from a CPU that is not the panic
 * owner) is discarded by the caller before it ever gets here, so that no such
 * write can acquire the lock or spend the terminal budget either. */
static void serial_emergency_emit(const char *buf, uint32_t len,
                                  int terminal, uint32_t *recov)
{
    uint64_t flags;
    int      locked;
    uint32_t i;
    /* Hoisted ONCE: the per-byte ownership retest below needs this, and deriving
     * it per byte would put a serializing CPUID -- a VM exit under KVM/WHPX --
     * inside the IRQ-disabled, lock-held region, once or twice for every byte of
     * every recoverable record. Only the latch is re-read per byte. */
    uint32_t me = (terminal ? 0u : (serial_emerg_self_cpu() & SERIAL_EMERG_CPU));

    /* local_irq_save rather than spin_lock_irqsave's machinery: trylock raises
     * no IRQL, so the release side must lower none (see spin_tryunlock). */
    flags  = local_irq_save();
    /* FOURTH acquisition of g_serial_lock, and the one most likely to be held by
     * a CPU that is about to park: this is the writer the panic path itself uses.
     * Ownership is recorded BY the acquire, in the same compare-exchange that
     * takes the lock, so serial_lock_release_if_owner() can always hand it back
     * and a failed try-lock leaves the real holder's word untouched.
     *
     * NOT the `me` above: that one is deliberately 0 on a terminal record so the
     * drop policy treats it as unowned. The lock owner must always be this CPU's
     * real identity, or the handback would compare against the wrong value. */
    locked = serial_emergency_acquire(
        &g_serial_lock, SERIAL_LOCK_OWNER_OF(serial_emerg_self_cpu()));

    serial_emergency_restore_lcr();

    for (i = 0; i < len; i++) {
        /* RECOVERABLE OUTPUT RE-TESTS OWNERSHIP PER BYTE.
         *
         * A survivable report checks ownership once before it starts, but an
         * epoch can arm at any point during a multi-chunk record -- and then its
         * remaining bytes interleave into a crash record that has just been
         * established. Re-testing per CHUNK is not enough: a chunk is up to
         * SERIAL_EMERG_CHUNK bytes, all of which would still land after the
         * arm. Per byte is the same granularity the ordinary path uses
         * (serial_putchar_raw's latch check), and it is the finest this can get:
         * the residual is the single byte between this test and its outb, which
         * no lock can close because serial_enter_emergency never takes
         * g_serial_lock and the emergency emitter deliberately proceeds when the
         * try-lock fails. So the bound is ONE byte, by design and by test.
         *
         * Tested before every PHYSICAL byte, not every input character: a '\n'
         * expands to '\r' plus '\n', so a single per-character test let TWO
         * bytes through after an arm and made the one-byte bound above false.
         *
         * Terminal output is exempt: it IS the crash record, and its callers are
         * already filtered by the routing predicate before they reach here. */
        if (!terminal && serial_emerg_drop_for_writer(me))
            break;
        if (buf[i] == '\n') {
            (void)serial_putchar_raw_bounded('\r', terminal, recov);
            if (!terminal && serial_emerg_reroute_should_drop())
                break;
        }
        (void)serial_putchar_raw_bounded(buf[i], terminal, recov);
    }

    /* ONE release: the store that frees the lock is the store that clears
     * ownership, so there is no longer a window between them for an abort to
     * land in. */
    serial_emergency_release(&g_serial_lock, locked);
    local_irq_restore(flags);
}

/* Marker appended when the caller's string could not be read to its end. An
 * empty tail would be indistinguishable from a short string, which on this path
 * is the difference between "that is all it said" and "the pointer describing
 * the crash was itself corrupt" -- the second is the more useful fact. */
static const char SERIAL_EMERG_FAULT_MARK[] = "<truncated: unreadable>";

/* The chunk walk, with its OUTPUT AS A PARAMETER.
 *
 * The arithmetic here -- how a record is cut into chunks, what the 1024-char
 * ceiling does to the last one, which stop reasons continue the walk, and
 * whether the unreadable marker lands after the partial text or instead of it --
 * had no test at all, because every byte left through the UART and there was no
 * seam to observe. A test that cannot see the chunk boundaries cannot tell a
 * correct walk from one that drops the final partial chunk or emits the marker
 * first.
 *
 * The seam is a PARAMETER, not an installable global. A file-static function
 * pointer would put MUTABLE CONTROL-FLOW STATE on the panic path: it could be
 * swapped between the calls that make up one panic report, so the record would
 * split across two destinations, and a corrupted pointer would turn the
 * diagnostic itself into a second fault. Passed as an argument, the production
 * call site names one constant callee the compiler can see through, and nothing
 * writable decides where a panic's output goes. */
static void serial_emergency_walk(const char *str, uint32_t ctx,
                                  serial_emerg_emit_fn emit, void *sink)
{
    uint32_t n       = 0;
    int      done    = 0;
    int      faulted = 0;

    if (!str || !emit) return;

    /* CAPTURE OUTSIDE THE LOCK, EMIT UNDER IT.
     *
     * The caller's pointer may itself be part of the corruption being reported,
     * so a load from it can fault. Holding g_serial_lock across that load is
     * what makes the fault unrecoverable: the nested fault runs the panic path
     * again, and on the async-isolation branch it publishes async_done and PARKS
     * this CPU -- still owning g_serial_lock. The system that branch is designed
     * to let survive then blocks forever on its next ordinary serial write.
     * Copying each bounded chunk first means a corrupted source faults while
     * this CPU owns nothing.
     *
     * Chunked rather than per-character so a record is still emitted in large
     * atomic pieces; per-character locking would interleave far worse under a
     * concurrent panic, and one 1024-byte buffer would be an unwelcome frame on
     * a #DF IST stack. */
    while (!done && n < SERIAL_EMERG_MAX_CHARS) {
        /* +1 for the terminator __kstr_read_guarded reserves inside its cap.
         * The emitted span is `k` bytes, so the extra byte is never written to
         * the UART -- it exists so the guarded loop can still copy a full
         * SERIAL_EMERG_CHUNK of payload. */
        char     chunk[SERIAL_EMERG_CHUNK_BUF];
        uint32_t k = 0;

        if (serial_emerg_ctx_allows_guarded_read(ctx)) {
            /* THE ONLY LOAD FROM CALLER MEMORY, and the one that can fault --
             * now ONE protected loop per chunk instead of a call per byte. The
             * cap above bounds how FAR this walks; it cannot make the walk
             * survive a pointer that is unmapped rather than unterminated. That
             * is what the guarded primitive adds: a #PF at its load is
             * redirected by page_fault_handler to a fixup, so a corrupt
             * description ends the record with a marker instead of taking the
             * machine down while it is trying to say why it died.
             *
             * NMI context does NOT take that path (the `else` below). Recovery
             * returns through IRETQ, which re-arms NMI delivery while the outer
             * NMI still owns IST2, so a second NMI would reuse that stack and
             * overwrite the frames -- strictly worse than the unguarded load,
             * whose fault goes terminal and never returns into the NMI handler
             * at all. The caller DECLARES this rather than the writer probing
             * for it: the vector is a hardware fact known at panic entry, and
             * any state a probe could consult here is exactly the state a panic
             * may have corrupted.
             *
             * NONCANON is reported as a fault for the same reason the per-byte
             * __kread_u8 returned failure on a non-canonical address: a source
             * running into the canonical hole is unreadable, not terminated. */
            uint32_t budget = SERIAL_EMERG_CHUNK;
            uint32_t stop   = KSTR_STOP_NUL;

            if (budget > SERIAL_EMERG_MAX_CHARS - n)
                budget = SERIAL_EMERG_MAX_CHARS - n;

            /* +1: the primitive reserves a terminator inside `cap`, and this
             * chunk is a byte span, not a C string -- the NUL is never emitted.
             * chunk[] is sized SERIAL_EMERG_CHUNK + 1 for exactly this. */
            k = __kstr_read_guarded(chunk, &str[n], budget + 1u, &stop);
            n += k;
            /* EXHAUSTIVE, and the default FAILS CLOSED. Only CAP continues the
             * walk, and only because it means the budget ran out with the
             * string still going. Treating an unrecognized reason as CAP is the
             * dangerous default here: a stop that returns k == 0 would leave n
             * unchanged and spin this loop forever, on the panic path, with the
             * UART as the only thing the machine still has. */
            switch (stop) {
            case KSTR_STOP_CAP:
                break;                       /* more string; next chunk */
            case KSTR_STOP_NUL:
                done = 1;
                break;
            case KSTR_STOP_FAULT:
            case KSTR_STOP_NONCANON:
            default:
                faulted = 1;
                done    = 1;
                break;
            }
            if (k)
                emit(sink, chunk, k);
            continue;
        }

        /* NMI context: the unguarded walk, one byte at a time. No fixup is
         * available here by design, so there is nothing a bounded protected
         * loop would buy -- and a raw `rep`-style copy would lose the
         * stop-at-terminator this needs. */
        while (k < SERIAL_EMERG_CHUNK && n < SERIAL_EMERG_MAX_CHARS) {
            char c = str[n];

            if (!c) { done = 1; break; }
            chunk[k++] = c;
            n++;
        }

        if (k)
            emit(sink, chunk, k);
    }

    /* AFTER the partial text, never instead of it. The bytes that were readable
     * are the evidence; the marker only says the source ran out before its
     * terminator did. */
    if (faulted)
        emit(sink, SERIAL_EMERG_FAULT_MARK,
             (uint32_t)(sizeof SERIAL_EMERG_FAULT_MARK - 1u));
}

/* The production sink: the UART, through the locked chunk emitter. `terminal`
 * and the recoverable per-call wait counter ride in the context rather than in
 * the walk's signature, because they are properties of WHO PAYS for the output
 * and not of how a record is cut into chunks -- which is precisely the split
 * that makes the walk testable without a UART. */
struct serial_emerg_uart_sink {
    int      terminal;
    uint32_t recov;
};

static void serial_emerg_emit_uart(void *sink, const char *buf, uint32_t len)
{
    struct serial_emerg_uart_sink *s = (struct serial_emerg_uart_sink *)sink;

    serial_emergency_emit(buf, len, s->terminal, &s->recov);
}

/* Shared body for the direct and rerouted string paths. */
static void serial_emergency_write_str(const char *str, int terminal, uint32_t ctx)
{
    struct serial_emerg_uart_sink sink = { .terminal = terminal, .recov = 0u };

    serial_emergency_walk(str, ctx, serial_emerg_emit_uart, &sink);
}

void serial_emerg_walk_for_test(const char *str, uint32_t ctx,
                                serial_emerg_emit_fn emit, void *sink)
{
    serial_emergency_walk(str, ctx, emit, sink);
}

void serial_write_emergency(const char *str)
{
    /* TERMINAL accounting, always -- NOT derived from the latch. The panic
     * reason and register dump are emitted through ~36 separate calls BEFORE
     * the latch is armed (arming waits for panic ownership), so deriving the
     * budget from the latch charged genuinely terminal output to the
     * recoverable per-call counter and reset it on every fragment, which meant
     * the bound depended on how the caller split its output. The latch decides
     * ROUTING; the entry point decides who pays. */
    serial_emergency_write_str(str, 1 /*terminal*/, PANIC_CTX_NORMAL);
}

void serial_write_emergency_ctx(const char *str, uint32_t ctx)
{
    serial_emergency_write_str(str, 1 /*terminal*/, ctx);
}

void serial_write_recoverable(const char *str)
{
    /* A survivable caller has no business writing into a crash record. If an
     * epoch is armed and this is not the owning CPU, drop -- the report is
     * recoverable by definition, the panic evidence is not. */
    if (serial_emerg_reroute_should_drop())
        return;

    /* The system SURVIVES this caller (today: the WER user-fault report), so it
     * gets the short per-byte wait and a call-local budget -- it can neither
     * spend the terminal allowance nor mask interrupts for the ~0.5 s the
     * terminal bound permits. */
    /* PANIC_CTX_UNKNOWN, NOT NORMAL. An earlier revision reasoned that "a
     * recoverable report is by definition emitted by a CPU that is not taking an
     * abort" and therefore hardcoded PANIC_CTX_NORMAL. That precondition is
     * false at every in-tree call site: idt.c's corrupt-CS and GS-invalid
     * branches run inside isr_handler for ANY vector, #NMI and #MC included, and
     * panic.c's async-isolation diagnostic runs inside panic_screen -- which had
     * already computed an NMI-aware context for its guarded appends and would
     * have discarded it here.
     *
     * Nothing breaks today because all three sites pass a literal or a stack
     * buffer, which cannot fault. The point is that the NEXT caller passing a
     * caller-supplied pointer would silently inherit the nested-NMI IST2 hazard,
     * and the restrictive default is what stops that being silent. A caller that
     * can prove its context uses serial_write_emergency_ctx. */
    serial_emergency_write_str(str, 0 /*recoverable*/, PANIC_CTX_UNKNOWN);
}

void serial_putchar_emergency(char c)
{
    uint32_t recov = 0;
    /* c is already a value, not a pointer, so there is nothing here that can
     * fault under the lock. Terminal accounting, same reasoning as
     * serial_write_emergency. */
    serial_emergency_emit(&c, 1u, 1 /*terminal*/, &recov);
}
